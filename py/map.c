#include <stdlib.h>
#include <assert.h>

#include "misc.h"
#include "mpconfig.h"
#include "qstr.h"
#include "obj.h"
#include "runtime0.h"

// approximatelly doubling primes; made with Mathematica command: Table[Prime[Floor[(1.7)^n]], {n, 3, 24}]
// prefixed with zero for the empty case.
STATIC int doubling_primes[] = {0, 7, 19, 43, 89, 179, 347, 647, 1229, 2297, 4243, 7829, 14347, 26017, 47149, 84947, 152443, 273253, 488399, 869927, 1547173, 2745121, 4861607};

STATIC int get_doubling_prime_greater_or_equal_to(int x) {
    for (int i = 0; i < sizeof(doubling_primes) / sizeof(int); i++) {
        if (doubling_primes[i] >= x) {
            return doubling_primes[i];
        }
    }
    // ran out of primes in the table!
    // return something sensible, at least make it odd
    return x | 1;
}

/******************************************************************************/
/* map                                                                        */

void mp_map_init(mp_map_t *map, int n) {
    if (n == 0) {
        map->alloc = 0;
        map->table = NULL;
    } else {
        map->alloc = get_doubling_prime_greater_or_equal_to(n + 1);
        map->table = m_new0(mp_map_elem_t, map->alloc);
    }
    map->used = 0;
    map->all_keys_are_qstrs = 1;
    map->table_is_fixed_array = 0;
}

void mp_map_init_fixed_table(mp_map_t *map, int n, const mp_obj_t *table) {
    map->alloc = n;
    map->used = n;
    map->all_keys_are_qstrs = 1;
    map->table_is_fixed_array = 1;
    map->table = (mp_map_elem_t*)table;
}

mp_map_t *mp_map_new(int n) {
    mp_map_t *map = m_new(mp_map_t, 1);
    mp_map_init(map, n);
    return map;
}

// Differentiate from mp_map_clear() - semantics is different
void mp_map_deinit(mp_map_t *map) {
    if (!map->table_is_fixed_array) {
        m_del(mp_map_elem_t, map->table, map->alloc);
    }
    map->used = map->alloc = 0;
}

void mp_map_free(mp_map_t *map) {
    mp_map_deinit(map);
    m_del_obj(mp_map_t, map);
}

void mp_map_clear(mp_map_t *map) {
    if (!map->table_is_fixed_array) {
        m_del(mp_map_elem_t, map->table, map->alloc);
    }
    map->alloc = 0;
    map->used = 0;
    map->all_keys_are_qstrs = 1;
    map->table_is_fixed_array = 0;
    map->table = NULL;
}

STATIC void mp_map_rehash(mp_map_t *map) {
    int old_alloc = map->alloc;
    mp_map_elem_t *old_table = map->table;
    map->alloc = get_doubling_prime_greater_or_equal_to(map->alloc + 1);
    map->used = 0;
    map->all_keys_are_qstrs = 1;
    map->table = m_new0(mp_map_elem_t, map->alloc);
    for (int i = 0; i < old_alloc; i++) {
        if (old_table[i].key != MP_OBJ_NULL && old_table[i].key != MP_OBJ_SENTINEL) {
            mp_map_lookup(map, old_table[i].key, MP_MAP_LOOKUP_ADD_IF_NOT_FOUND)->value = old_table[i].value;
        }
    }
    m_del(mp_map_elem_t, old_table, old_alloc);
}

// MP_MAP_LOOKUP behaviour:
//  - returns NULL if not found, else the slot it was found in with key,value non-null
// MP_MAP_LOOKUP_ADD_IF_NOT_FOUND behaviour:
//  - returns slot, with key non-null and value=MP_OBJ_NULL if it was added
// MP_MAP_LOOKUP_REMOVE_IF_FOUND behaviour:
//  - returns NULL if not found, else the slot if was found in with key null and value non-null
mp_map_elem_t* mp_map_lookup(mp_map_t *map, mp_obj_t index, mp_map_lookup_kind_t lookup_kind) {
    // if the map is a fixed array then we must do a brute force linear search
    if (map->table_is_fixed_array) {
        if (lookup_kind != MP_MAP_LOOKUP) {
            return NULL;
        }
        for (mp_map_elem_t *elem = &map->table[0], *top = &map->table[map->used]; elem < top; elem++) {
            if (elem->key == index || (!map->all_keys_are_qstrs && mp_obj_equal(elem->key, index))) {
                return elem;
            }
        }
        return NULL;
    }

    // map is a hash table (not a fixed array), so do a hash lookup

    if (map->alloc == 0) {
        if (lookup_kind & MP_MAP_LOOKUP_ADD_IF_NOT_FOUND) {
            mp_map_rehash(map);
        } else {
            return NULL;
        }
    }

    machine_uint_t hash = mp_obj_hash(index);
    uint pos = hash % map->alloc;
    uint start_pos = pos;
    mp_map_elem_t *avail_slot = NULL;
    for (;;) {
        mp_map_elem_t *slot = &map->table[pos];
        if (slot->key == MP_OBJ_NULL) {
            // found NULL slot, so index is not in table
            if (lookup_kind & MP_MAP_LOOKUP_ADD_IF_NOT_FOUND) {
                map->used += 1;
                if (avail_slot == NULL) {
                    avail_slot = slot;
                }
                slot->key = index;
                slot->value = MP_OBJ_NULL;
                if (!MP_OBJ_IS_QSTR(index)) {
                    map->all_keys_are_qstrs = 0;
                }
                return slot;
            } else {
                return NULL;
            }
        } else if (slot->key == MP_OBJ_SENTINEL) {
            // found deleted slot, remember for later
            if (avail_slot == NULL) {
                avail_slot = slot;
            }
        } else if (slot->key == index || (!map->all_keys_are_qstrs && mp_obj_equal(slot->key, index))) {
            // found index
            // Note: CPython does not replace the index; try x={True:'true'};x[1]='one';x
            if (lookup_kind & MP_MAP_LOOKUP_REMOVE_IF_FOUND) {
                // delete element in this slot
                map->used--;
                if (map->table[(pos + 1) % map->alloc].key == MP_OBJ_NULL) {
                    // optimisation if next slot is empty
                    slot->key = MP_OBJ_NULL;
                } else {
                    slot->key = MP_OBJ_SENTINEL;
                }
                // keep slot->value so that caller can access it if needed
            }
            return slot;
        }

        // not yet found, keep searching in this table
        pos = (pos + 1) % map->alloc;

        if (pos == start_pos) {
            // search got back to starting position, so index is not in table
            if (lookup_kind & MP_MAP_LOOKUP_ADD_IF_NOT_FOUND) {
                if (avail_slot != NULL) {
                    // there was an available slot, so use that
                    map->used++;
                    avail_slot->key = index;
                    avail_slot->value = MP_OBJ_NULL;
                    if (!MP_OBJ_IS_QSTR(index)) {
                        map->all_keys_are_qstrs = 0;
                    }
                    return avail_slot;
                } else {
                    // not enough room in table, rehash it
                    mp_map_rehash(map);
                    // restart the search for the new element
                    start_pos = pos = hash % map->alloc;
                }
            } else {
                return NULL;
            }
        }
    }
}

/******************************************************************************/
/* set                                                                        */

void mp_set_init(mp_set_t *set, int n) {
    set->alloc = get_doubling_prime_greater_or_equal_to(n + 1);
    set->used = 0;
    set->table = m_new0(mp_obj_t, set->alloc);
}

STATIC void mp_set_rehash(mp_set_t *set) {
    int old_alloc = set->alloc;
    mp_obj_t *old_table = set->table;
    set->alloc = get_doubling_prime_greater_or_equal_to(set->alloc + 1);
    set->used = 0;
    set->table = m_new0(mp_obj_t, set->alloc);
    for (int i = 0; i < old_alloc; i++) {
        if (old_table[i] != MP_OBJ_NULL && old_table[i] != MP_OBJ_SENTINEL) {
            mp_set_lookup(set, old_table[i], MP_MAP_LOOKUP_ADD_IF_NOT_FOUND);
        }
    }
    m_del(mp_obj_t, old_table, old_alloc);
}

mp_obj_t mp_set_lookup(mp_set_t *set, mp_obj_t index, mp_map_lookup_kind_t lookup_kind) {
    if (set->alloc == 0) {
        if (lookup_kind & MP_MAP_LOOKUP_ADD_IF_NOT_FOUND) {
            mp_set_rehash(set);
        } else {
            return NULL;
        }
    }
    machine_uint_t hash = mp_obj_hash(index);
    uint pos = hash % set->alloc;
    uint start_pos = pos;
    mp_obj_t *avail_slot = NULL;
    for (;;) {
        mp_obj_t elem = set->table[pos];
        if (elem == MP_OBJ_NULL) {
            // found NULL slot, so index is not in table
            if (lookup_kind & MP_MAP_LOOKUP_ADD_IF_NOT_FOUND) {
                if (avail_slot == NULL) {
                    avail_slot = &set->table[pos];
                }
                set->used++;
                *avail_slot = index;
                return index;
            } else {
                return MP_OBJ_NULL;
            }
        } else if (elem == MP_OBJ_SENTINEL) {
            // found deleted slot, remember for later
            if (avail_slot == NULL) {
                avail_slot = &set->table[pos];
            }
        } else if (mp_obj_equal(elem, index)) {
            // found index
            if (lookup_kind & MP_MAP_LOOKUP_REMOVE_IF_FOUND) {
                // delete element
                set->used--;
                if (set->table[(pos + 1) % set->alloc] == MP_OBJ_NULL) {
                    // optimisation if next slot is empty
                    set->table[pos] = MP_OBJ_NULL;
                } else {
                    set->table[pos] = MP_OBJ_SENTINEL;
                }
            }
            return elem;
        }

        // not yet found, keep searching in this table
        pos = (pos + 1) % set->alloc;

        if (pos == start_pos) {
            // search got back to starting position, so index is not in table
            if (lookup_kind & MP_MAP_LOOKUP_ADD_IF_NOT_FOUND) {
                if (avail_slot != NULL) {
                    // there was an available slot, so use that
                    set->used++;
                    *avail_slot = index;
                    return index;
                } else {
                    // not enough room in table, rehash it
                    mp_set_rehash(set);
                    // restart the search for the new element
                    start_pos = pos = hash % set->alloc;
                }
            } else {
                return MP_OBJ_NULL;
            }
        }
    }
}

mp_obj_t mp_set_remove_first(mp_set_t *set) {
    for (uint pos = 0; pos < set->alloc; pos++) {
        if (MP_SET_SLOT_IS_FILLED(set, pos)) {
            mp_obj_t elem = set->table[pos];
            // delete element
            set->used--;
            if (set->table[(pos + 1) % set->alloc] == MP_OBJ_NULL) {
                // optimisation if next slot is empty
                set->table[pos] = MP_OBJ_NULL;
            } else {
                set->table[pos] = MP_OBJ_SENTINEL;
            }
            return elem;
        }
    }
    return MP_OBJ_NULL;
}

void mp_set_clear(mp_set_t *set) {
    m_del(mp_obj_t, set->table, set->alloc);
    set->alloc = 0;
    set->used = 0;
    set->table = NULL;
}

#if DEBUG_PRINT
void mp_map_dump(mp_map_t *map) {
    for (int i = 0; i < map->alloc; i++) {
        if (map->table[i].key != NULL) {
            mp_obj_print(map->table[i].key, PRINT_REPR);
        } else {
            printf("(nil)");
        }
        printf(": %p\n", map->table[i].value);
    }
    printf("---\n");
}
#endif

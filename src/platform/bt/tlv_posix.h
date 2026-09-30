#ifndef _BLE_TLV_POSIX_
#define _BLE_TLV_POSIX_

#include <stdint.h>
#include <stdio.h>
#include <btstack/btstack_tlv.h>
#include <btstack/btstack_linked_list.h>

#if defined __cplusplus
extern "C" {
#endif

typedef struct {
	btstack_linked_list_t entry_list;
	const char *db_path;
} tlv_posix_t;

const btstack_tlv_t* tlv_posix_init_instance(tlv_posix_t *context, const char *db_path);
void tlv_posix_deinit(tlv_posix_t *self);

#if defined __cplusplus
}
#endif
#endif

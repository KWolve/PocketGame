#define BTSTACK_FILE__ "tlv_posix.c"

#include "tlv_posix.h"
#include <btstack/btstack_debug.h>
#include <btstack/btstack_util.h>

#include <stdlib.h>
#include <string.h>

#define TLV_HEADER_LEN     8
#define MAX_TLV_VALUE_SIZE 2048

static const char *tlv_header_magic = "BTstack";

#define DUMMY_SIZE 4
typedef struct tlv_entry {
	void   * next;
	uint32_t tag;
	uint32_t len;
	uint8_t  value[DUMMY_SIZE];	// dummy size
} tlv_entry_t;

static void tlv_posix_write_db(tlv_posix_t *self) {
	FILE *file = fopen(self->db_path, "w");
	if (!file) {
		log_error("fopen failed");
		return;
	}

	char magic[TLV_HEADER_LEN] = { 0 };
	strcpy(magic, tlv_header_magic);
	if (fwrite(magic, 1, sizeof(magic), file) != sizeof(magic)) {
		log_error("write magic failed");
		fclose(file);
		return;
	}

	btstack_linked_list_iterator_t it;
	btstack_linked_list_iterator_init(&it, &self->entry_list);
	while (btstack_linked_list_iterator_has_next(&it)) {
		tlv_entry_t *entry = (tlv_entry_t *) btstack_linked_list_iterator_next(&it);

		uint8_t header[8];
		big_endian_store_32(header, 0, entry->tag);
		big_endian_store_32(header, 4, entry->len);
		if (fwrite(header, 1, sizeof(header), file) != sizeof(header)) {
			log_error("write header failed");
			break;
		}

		if (entry->len > 0) {
			if (fwrite(&entry->value[0], 1, entry->len, file) != entry->len) {
				log_error("write data failed");
				break;
			}
		}
	}

	fflush(file);
	fclose(file);

	//log_info("write db ok");
}

static tlv_entry_t* tlv_posix_find_entry(tlv_posix_t *self, uint32_t tag) {
	btstack_linked_list_iterator_t it;
	btstack_linked_list_iterator_init(&it, &self->entry_list);
	while (btstack_linked_list_iterator_has_next(&it)) {
		tlv_entry_t *entry = (tlv_entry_t *) btstack_linked_list_iterator_next(&it);
		if (entry->tag != tag) continue;
		return entry;
	}
	return NULL;
}

static void tlv_posix_delete_tag(void *context, uint32_t tag) {
	tlv_posix_t *self = (tlv_posix_t *) context;
	btstack_linked_list_iterator_t it;
	btstack_linked_list_iterator_init(&it, &self->entry_list);
	while (btstack_linked_list_iterator_has_next(&it)) {
		tlv_entry_t *entry = (tlv_entry_t *) btstack_linked_list_iterator_next(&it);
		if (entry->tag != tag) {
			continue;
		}

		btstack_linked_list_iterator_remove(&it);
		free(entry);

		tlv_posix_write_db(self);
		return;
	}
}

static int tlv_posix_get_tag(void *context, uint32_t tag, uint8_t *buffer, uint32_t buffer_size){
	tlv_posix_t *self = (tlv_posix_t *) context;
	tlv_entry_t *entry = tlv_posix_find_entry(self, tag);
	// not found
	if (!entry) return 0;
	// return len if buffer = NULL
	if (!buffer) return entry->len;
	// otherwise copy data into buffer
	uint16_t bytes_to_copy = btstack_min(buffer_size, entry->len);
	memcpy(buffer, &entry->value[0], bytes_to_copy);
	return bytes_to_copy;
}

static int tlv_posix_store_tag(void *context, uint32_t tag, const uint8_t *data, uint32_t data_size) {
	tlv_posix_t *self = (tlv_posix_t *) context;

	// enforce arbitrary max value size
	btstack_assert(data_size <= MAX_TLV_VALUE_SIZE);

	tlv_entry_t *old_entry = tlv_posix_find_entry(self, tag);
	if (old_entry) {
		btstack_linked_list_remove(&self->entry_list, (btstack_linked_item_t *) old_entry);
		free(old_entry);
	}

	uint32_t entry_size = sizeof(tlv_entry_t) - DUMMY_SIZE + data_size;
	tlv_entry_t *new_entry = (tlv_entry_t *) malloc(entry_size);
	if (!new_entry) return 0;
	memset(new_entry, 0, entry_size);
	new_entry->tag = tag;
	new_entry->len = data_size;
	memcpy(&new_entry->value[0], data, data_size);

	btstack_linked_list_add(&self->entry_list, (btstack_linked_item_t *) new_entry);
	tlv_posix_write_db(self);

	return 0;
}

static int tlv_posix_read_db(tlv_posix_t *self) {
	log_info("open db %s", self->db_path);
	FILE *file = fopen(self->db_path, "r");
	if (!file) {
		return -1;
	}

	uint8_t header[TLV_HEADER_LEN];
	if (fread(header, 1, TLV_HEADER_LEN, file) == TLV_HEADER_LEN) {
		if (memcmp(header, tlv_header_magic, strlen(tlv_header_magic)) == 0) {
			log_info("BTstack Magic Header found");
			while (true) {
				uint8_t entry[8];
				size_t entries_read = fread(entry, 1, sizeof(entry), file);
				if (entries_read == 0) {
					// EOF, we're good
					break;
				}
				if (entries_read != sizeof(entry)) {
					break;
				}

				uint32_t tag = big_endian_read_32(entry, 0);
				uint32_t len = big_endian_read_32(entry, 4);

				// arbitrary safety check: values <= MAX_TLV_VALUE_SIZE
				if (len > MAX_TLV_VALUE_SIZE) {
					break;
				}

				// create new entry for regular tag
				tlv_entry_t *new_entry = NULL;
				if (len > 0) {
					new_entry = (tlv_entry_t *) malloc(sizeof(tlv_entry_t) - DUMMY_SIZE + len);
					if (!new_entry) {
						break;
					}
					new_entry->tag = tag;
					new_entry->len = len;

					size_t value_read = fread(&new_entry->value[0], 1, len, file);
					if (value_read != len) {
						break;
					}
				}

				tlv_entry_t *old_entry = tlv_posix_find_entry(self, tag);
				if (old_entry) {
					btstack_linked_list_remove(&self->entry_list, (btstack_linked_item_t *) old_entry);
					free(old_entry);
				}

				if (new_entry) {
					btstack_linked_list_add(&self->entry_list, (btstack_linked_item_t *) new_entry);
				}
			}
		}
	}

	fclose(file);
	return 0;
}

static const btstack_tlv_t tlv_posix = {
	/* int  (*get_tag)(..);     */ &tlv_posix_get_tag,
	/* int (*store_tag)(..);    */ &tlv_posix_store_tag,
	/* void (*delete_tag)(v..); */ &tlv_posix_delete_tag,
};

const btstack_tlv_t* tlv_posix_init_instance(tlv_posix_t *self, const char *db_path) {
	memset(self, 0, sizeof(tlv_posix_t));
	self->db_path = db_path;

	if (db_path != NULL) {
		tlv_posix_read_db(self);
	}
	return &tlv_posix;
}

void tlv_posix_deinit(tlv_posix_t *self) {
	btstack_linked_list_iterator_t it;
	btstack_linked_list_iterator_init(&it, &self->entry_list);
	while (btstack_linked_list_iterator_has_next(&it)) {
		tlv_entry_t *entry = (tlv_entry_t *) btstack_linked_list_iterator_next(&it);
		btstack_linked_list_iterator_remove(&it);
		free(entry);
	}
}

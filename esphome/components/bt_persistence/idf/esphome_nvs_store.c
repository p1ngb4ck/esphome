#include "osi/esphome_nvs_store.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#if CONFIG_SPIRAM
#define STORE_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define STORE_CAPS MALLOC_CAP_8BIT
#endif

#define NAME_MAX_LEN (NVS_KEY_NAME_MAX_SIZE - 1)
#define IMAGE_MAGIC 0x564B5442u  // "BTKV"
#define IMAGE_VERSION 1u
#define IMAGE_HEADER 12u

typedef struct store_ns {
  struct store_ns *next;
  uint32_t id;
  char name[NVS_KEY_NAME_MAX_SIZE];
} store_ns_t;

typedef struct store_entry {
  struct store_entry *next;
  uint32_t ns;
  char key[NVS_KEY_NAME_MAX_SIZE];
  size_t len;
  uint8_t data[];
} store_entry_t;

typedef struct {
  store_ns_t *ns;
  store_entry_t *entries;
  uint32_t next_id;
} store_t;

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static store_t s_store;
static bool s_loaded;
static uint32_t s_generation;

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void changed(void) { __atomic_add_fetch(&s_generation, 1, __ATOMIC_RELEASE); }

static bool name_ok(const char *name) { return name != NULL && name[0] != '\0' && strlen(name) <= NAME_MAX_LEN; }

static void store_free(store_t *store) {
  while (store->entries != NULL) {
    store_entry_t *next = store->entries->next;
    heap_caps_free(store->entries);
    store->entries = next;
  }
  while (store->ns != NULL) {
    store_ns_t *next = store->ns->next;
    heap_caps_free(store->ns);
    store->ns = next;
  }
  store->next_id = 1;
}

static store_ns_t *ns_find(store_t *store, const char *name) {
  for (store_ns_t *ns = store->ns; ns != NULL; ns = ns->next) {
    if (strcmp(ns->name, name) == 0)
      return ns;
  }
  return NULL;
}

static store_ns_t *ns_add(store_t *store, const char *name) {
  store_ns_t *ns = heap_caps_calloc(1, sizeof(store_ns_t), STORE_CAPS);
  if (ns == NULL)
    return NULL;
  ns->id = store->next_id++;
  strcpy(ns->name, name);
  ns->next = store->ns;
  store->ns = ns;
  return ns;
}

static store_entry_t **entry_find(store_t *store, uint32_t ns, const char *key) {
  for (store_entry_t **e = &store->entries; *e != NULL; e = &(*e)->next) {
    if ((*e)->ns == ns && strcmp((*e)->key, key) == 0)
      return e;
  }
  return NULL;
}

static store_entry_t *entry_new(uint32_t ns, const char *key, const void *value, size_t len) {
  store_entry_t *e = heap_caps_malloc(sizeof(store_entry_t) + len, STORE_CAPS);
  if (e == NULL)
    return NULL;
  e->next = NULL;
  e->ns = ns;
  memset(e->key, 0, sizeof(e->key));
  strcpy(e->key, key);
  e->len = len;
  if (len != 0)
    memcpy(e->data, value, len);
  return e;
}

// Handle: namespace id << 1 | writable.
static esp_err_t check_handle(nvs_handle_t handle, bool write, uint32_t *ns) {
  if (!s_loaded)
    return ESP_ERR_NVS_NOT_INITIALIZED;
  if ((handle >> 1) == 0)
    return ESP_ERR_NVS_INVALID_HANDLE;
  if (write && (handle & 1u) == 0)
    return ESP_ERR_NVS_READ_ONLY;
  *ns = handle >> 1;
  return ESP_OK;
}

esp_err_t esphome_nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out_handle) {
  if (s_lock == NULL || out_handle == NULL)
    return ESP_ERR_NVS_NOT_INITIALIZED;
  if (!name_ok(name))
    return ESP_ERR_NVS_KEY_TOO_LONG;
  lock();
  esp_err_t err = ESP_OK;
  if (!s_loaded) {
    err = ESP_ERR_NVS_NOT_INITIALIZED;
  } else {
    store_ns_t *ns = ns_find(&s_store, name);
    if (ns == NULL && mode == NVS_READWRITE)
      ns = ns_add(&s_store, name);
    if (ns == NULL) {
      err = mode == NVS_READWRITE ? ESP_ERR_NO_MEM : ESP_ERR_NVS_NOT_FOUND;
    } else {
      *out_handle = (ns->id << 1) | (mode == NVS_READWRITE ? 1u : 0u);
    }
  }
  unlock();
  return err;
}

void esphome_nvs_close(nvs_handle_t handle) { (void) handle; }

esp_err_t esphome_nvs_commit(nvs_handle_t handle) {
  uint32_t ns;
  return check_handle(handle, false, &ns);
}

esp_err_t esphome_nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length) {
  if (s_lock == NULL)
    return ESP_ERR_NVS_NOT_INITIALIZED;
  if (!name_ok(key))
    return ESP_ERR_NVS_KEY_TOO_LONG;
  lock();
  uint32_t ns;
  esp_err_t err = check_handle(handle, false, &ns);
  if (err == ESP_OK) {
    store_entry_t **e = entry_find(&s_store, ns, key);
    if (e == NULL) {
      err = ESP_ERR_NVS_NOT_FOUND;
    } else if (length == NULL) {
      err = ESP_ERR_NVS_INVALID_LENGTH;
    } else if (out_value == NULL) {
      *length = (*e)->len;
    } else if (*length < (*e)->len) {
      *length = (*e)->len;
      err = ESP_ERR_NVS_INVALID_LENGTH;
    } else {
      *length = (*e)->len;
      memcpy(out_value, (*e)->data, (*e)->len);
    }
  }
  unlock();
  return err;
}

esp_err_t esphome_nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length) {
  if (s_lock == NULL)
    return ESP_ERR_NVS_NOT_INITIALIZED;
  if (!name_ok(key))
    return ESP_ERR_NVS_KEY_TOO_LONG;
  if (value == NULL && length != 0)
    return ESP_ERR_NVS_INVALID_LENGTH;
  lock();
  uint32_t ns;
  esp_err_t err = check_handle(handle, true, &ns);
  if (err == ESP_OK) {
    store_entry_t **e = entry_find(&s_store, ns, key);
    if (e != NULL && (*e)->len == length && (length == 0 || memcmp((*e)->data, value, length) == 0)) {
      unlock();
      return ESP_OK;
    }
    store_entry_t *fresh = entry_new(ns, key, value, length);
    if (fresh == NULL) {
      err = ESP_ERR_NO_MEM;
    } else {
      if (e != NULL) {
        store_entry_t *old = *e;
        fresh->next = old->next;
        *e = fresh;
        heap_caps_free(old);
      } else {
        fresh->next = s_store.entries;
        s_store.entries = fresh;
      }
      changed();
    }
  }
  unlock();
  return err;
}

esp_err_t esphome_nvs_erase_key(nvs_handle_t handle, const char *key) {
  if (s_lock == NULL)
    return ESP_ERR_NVS_NOT_INITIALIZED;
  if (!name_ok(key))
    return ESP_ERR_NVS_KEY_TOO_LONG;
  lock();
  uint32_t ns;
  esp_err_t err = check_handle(handle, true, &ns);
  if (err == ESP_OK) {
    store_entry_t **e = entry_find(&s_store, ns, key);
    if (e == NULL) {
      err = ESP_ERR_NVS_NOT_FOUND;
    } else {
      store_entry_t *old = *e;
      *e = old->next;
      heap_caps_free(old);
      changed();
    }
  }
  unlock();
  return err;
}

esp_err_t esphome_nvs_erase_all(nvs_handle_t handle) {
  if (s_lock == NULL)
    return ESP_ERR_NVS_NOT_INITIALIZED;
  lock();
  uint32_t ns;
  esp_err_t err = check_handle(handle, true, &ns);
  if (err == ESP_OK) {
    bool removed = false;
    store_entry_t **e = &s_store.entries;
    while (*e != NULL) {
      if ((*e)->ns == ns) {
        store_entry_t *old = *e;
        *e = old->next;
        heap_caps_free(old);
        removed = true;
      } else {
        e = &(*e)->next;
      }
    }
    if (removed)
      changed();
  }
  unlock();
  return err;
}

bool esphome_nvs_store_init(void) {
  if (s_lock == NULL) {
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    s_store.next_id = 1;
  }
  return s_lock != NULL;
}

uint32_t esphome_nvs_store_generation(void) { return __atomic_load_n(&s_generation, __ATOMIC_ACQUIRE); }

static void put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t) v;
  p[1] = (uint8_t) (v >> 8);
  p[2] = (uint8_t) (v >> 16);
  p[3] = (uint8_t) (v >> 24);
}

static uint32_t get_u32(const uint8_t *p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static const char *ns_name(store_t *store, uint32_t id) {
  for (store_ns_t *ns = store->ns; ns != NULL; ns = ns->next) {
    if (ns->id == id)
      return ns->name;
  }
  return NULL;
}

// Image: "BTKV", version, 3 reserved, entry count, then per entry namespace length + name, key
// length + key, data length + data, all little endian, closed by a CRC32 over everything before.
bool esphome_nvs_store_serialize(uint8_t *buf, size_t cap, size_t *len, uint32_t *generation) {
  if (s_lock == NULL || len == NULL)
    return false;
  lock();
  size_t need = IMAGE_HEADER + 4;
  uint32_t count = 0;
  for (store_entry_t *e = s_store.entries; e != NULL; e = e->next) {
    const char *ns = ns_name(&s_store, e->ns);
    need += 1 + strlen(ns) + 1 + strlen(e->key) + 4 + e->len;
    count++;
  }
  *len = need;
  if (buf == NULL || cap < need) {
    unlock();
    return false;
  }
  if (generation != NULL)
    *generation = esphome_nvs_store_generation();
  put_u32(buf, IMAGE_MAGIC);
  buf[4] = IMAGE_VERSION;
  buf[5] = buf[6] = buf[7] = 0;
  put_u32(buf + 8, count);
  uint8_t *p = buf + IMAGE_HEADER;
  for (store_entry_t *e = s_store.entries; e != NULL; e = e->next) {
    const char *ns = ns_name(&s_store, e->ns);
    size_t n = strlen(ns);
    *p++ = (uint8_t) n;
    memcpy(p, ns, n);
    p += n;
    n = strlen(e->key);
    *p++ = (uint8_t) n;
    memcpy(p, e->key, n);
    p += n;
    put_u32(p, (uint32_t) e->len);
    p += 4;
    memcpy(p, e->data, e->len);
    p += e->len;
  }
  put_u32(p, esp_rom_crc32_le(0, buf, (uint32_t) (p - buf)));
  unlock();
  return true;
}

static bool read_name(const uint8_t **p, const uint8_t *end, char *out) {
  if (*p >= end)
    return false;
  size_t n = **p;
  (*p)++;
  if (n == 0 || n > NAME_MAX_LEN || (size_t) (end - *p) < n)
    return false;
  memcpy(out, *p, n);
  out[n] = '\0';
  *p += n;
  return true;
}

bool esphome_nvs_store_load(const uint8_t *data, size_t len) {
  if (s_lock == NULL)
    return false;
  store_t fresh = {NULL, NULL, 1};
  if (len != 0) {
    if (data == NULL || len < IMAGE_HEADER + 4 || get_u32(data) != IMAGE_MAGIC || data[4] != IMAGE_VERSION)
      return false;
    const uint8_t *end = data + len - 4;
    if (esp_rom_crc32_le(0, data, (uint32_t) (end - data)) != get_u32(end))
      return false;
    uint32_t count = get_u32(data + 8);
    const uint8_t *p = data + IMAGE_HEADER;
    store_entry_t **tail = &fresh.entries;
    for (uint32_t i = 0; i < count; i++) {
      char ns_buf[NVS_KEY_NAME_MAX_SIZE];
      char key[NVS_KEY_NAME_MAX_SIZE];
      if (!read_name(&p, end, ns_buf) || !read_name(&p, end, key) || end - p < 4)
        goto fail;
      size_t n = get_u32(p);
      p += 4;
      if ((size_t) (end - p) < n)
        goto fail;
      store_ns_t *ns = ns_find(&fresh, ns_buf);
      if (ns == NULL && (ns = ns_add(&fresh, ns_buf)) == NULL)
        goto fail;
      store_entry_t *e = entry_new(ns->id, key, p, n);
      if (e == NULL)
        goto fail;
      *tail = e;
      tail = &e->next;
      p += n;
    }
    if (p != end)
      goto fail;
  }
  lock();
  store_free(&s_store);
  s_store = fresh;
  s_loaded = true;
  unlock();
  return true;

fail:
  store_free(&fresh);
  return false;
}

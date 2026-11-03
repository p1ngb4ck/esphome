#include "package_manager.h"

#if defined(USE_ESP_IDF) && defined(USE_PACKAGE_MANAGER) && defined(USE_JSON)

#include <sdkconfig.h>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace esphome::package_manager {

void PackageManager::dump_installed(JsonObject root) {
  JsonArray list = root["packages"].to<JsonArray>();
  this->for_each([&list](const Package &p) {
    JsonObject e = list.add<JsonObject>();
    e["name"] = p.name;
    e["version"] = p.version;
    e["flash"] = p.size;
    e["psram"] = p.psram;
    e["iram"] = p.iram;
    e["psram_used"] = p.module.data_bytes();
    e["recovery"] = (p.recovery & RECOVERY_BIT) != 0;
    e["hold"] = (p.recovery & HOLD_BIT) != 0;
    e["abi"] = p.abi;
    e["enabled"] = p.enabled != 0;
    e["running"] = p.module.ok();
    e["settings"] = p.settings == SettingsTarget::STORAGE ? "storage" : "preferences";
  });
  root["directory"] = this->directory_ != nullptr ? this->directory_ : "";
  root["installing"] = this->installing_name();
  root["last_error"] = this->last_error();
}

template<typename F> static void each_name(const char *list, F &&fn) {
  char name[sizeof(StoreEntry::name)];
  const char *p = list;
  while (*p != '\0') {
    size_t n = 0;
    for (; *p != '\0' && *p != ','; p++) {
      if (n + 1 < sizeof(name))
        name[n++] = *p;
    }
    name[n] = '\0';
    if (*p == ',')
      p++;
    if (n > 0)
      fn(static_cast<const char *>(name));
  }
}

static bool lists(const char *list, const char *name) {
  bool found = false;
  each_name(list, [&found, name](const char *dep) { found = found || std::strcmp(dep, name) == 0; });
  return found;
}

void PackageManager::dump_info(const std::string &name, JsonObject root) {
  root["name"] = name;
  bool known = false;
  this->for_each([&](const Package &p) {
    if (name != p.name)
      return;
    known = true;
    JsonObject e = root["installed"].to<JsonObject>();
    e["version"] = p.version;
    e["depends"] = p.depends;
    e["flash"] = p.size;
    e["code"] = p.length;
    e["psram"] = p.psram;
    e["psram_used"] = p.module.data_bytes();
    e["iram"] = p.iram;
    e["abi"] = p.abi;
    e["enabled"] = p.enabled != 0;
    e["running"] = p.module.ok();
    e["recovery"] = (p.recovery & RECOVERY_BIT) != 0;
    e["hold"] = (p.recovery & HOLD_BIT) != 0;
    e["settings"] = p.settings == SettingsTarget::STORAGE ? "storage" : "preferences";
  });
#ifdef USE_PACKAGE_MANAGER_HTTP
  this->for_each_available([&](const AvailablePackage &p, const Repository &r) {
    if (name != p.name)
      return;
    known = true;
    JsonObject e = root["available"].to<JsonObject>();
    e["version"] = p.version;
    e["repository"] = r.name;
    e["filename"] = p.filename;
    e["size"] = p.size;
    e["flash"] = p.flash;
    e["psram"] = p.psram;
    e["abi"] = p.abi;
    e["depends"] = p.depends;
    e["description"] = p.description;
    e["sha256"] = p.has_sha256 != 0;
  });
#endif
  root["known"] = known;
}

void PackageManager::dump_find(const std::string &query, JsonObject root) {
  JsonArray list = root["packages"].to<JsonArray>();
  auto matches = [&query](const char *text) { return query.empty() || std::strstr(text, query.c_str()) != nullptr; };
  this->for_each([&](const Package &p) {
    if (!matches(p.name))
      return;
    JsonObject e = list.add<JsonObject>();
    e["name"] = p.name;
    e["version"] = p.version;
    e["installed"] = true;
  });
#ifdef USE_PACKAGE_MANAGER_HTTP
  this->for_each_available([&](const AvailablePackage &p, const Repository &r) {
    if (!matches(p.name) && !matches(p.description))
      return;
    JsonObject e = list.add<JsonObject>();
    e["name"] = p.name;
    e["version"] = p.version;
    e["repository"] = r.name;
    e["description"] = p.description;
    e["installed"] = false;
  });
#endif
}

void PackageManager::dump_depends(const std::string &name, bool reverse, bool recursive, JsonObject root) {
  root["name"] = name;
  JsonArray list = root[reverse ? "needed_by" : "needs"].to<JsonArray>();
  if (!reverse) {
    std::string depends;
    this->for_each([&](const Package &p) {
      if (name == p.name)
        depends = p.depends;
    });
#ifdef USE_PACKAGE_MANAGER_HTTP
    if (depends.empty()) {
      this->for_each_available([&](const AvailablePackage &p, const Repository &) {
        if (name == p.name)
          depends = p.depends;
      });
    }
#endif
    each_name(depends.c_str(), [&list](const char *dep) { list.add(std::string(dep)); });
    return;
  }
  char found[STORE_MAX_ENTRIES][sizeof(StoreEntry::name)];
  size_t count = 0;
  for (size_t pass = 0; pass <= count; pass++) {
    const char *target = pass == 0 ? name.c_str() : found[pass - 1];
    this->for_each([&](const Package &p) {
      if (count >= STORE_MAX_ENTRIES || !lists(p.depends, target) || name == p.name)
        return;
      for (size_t i = 0; i < count; i++) {
        if (std::strcmp(found[i], p.name) == 0)
          return;
      }
      std::strncpy(found[count], p.name, sizeof(found[count]) - 1);
      found[count][sizeof(found[count]) - 1] = '\0';
      count++;
    });
    if (!recursive)
      break;
  }
  for (size_t i = 0; i < count; i++)
    list.add(std::string(found[i]));
}

void PackageManager::dump_architecture(JsonObject root) {
  char id[9];
  snprintf(id, sizeof(id), "%08" PRIx32, this->firmware_id());
  root["architecture"] = CONFIG_IDF_TARGET;
  root["firmware_id"] = id;
}

#ifdef USE_PACKAGE_MANAGER_HTTP
static const char *repo_state(RepoState state) {
  switch (state) {
    case RepoState::OK:
      return "ok";
    case RepoState::FAILED:
      return "failed";
    default:
      return "none";
  }
}

void PackageManager::dump_available(JsonObject root) {
  JsonArray repos = root["repositories"].to<JsonArray>();
  this->for_each_repository([&repos](const Repository &r) {
    JsonObject e = repos.add<JsonObject>();
    e["name"] = r.name;
    e["url"] = r.url;
    e["enabled"] = r.enabled != 0;
    e["builtin"] = r.builtin != 0;
    e["state"] = repo_state(r.state);
    e["count"] = r.count;
  });
  JsonArray list = root["packages"].to<JsonArray>();
  this->for_each_available([this, &list](const AvailablePackage &p, const Repository &r) {
    JsonObject e = list.add<JsonObject>();
    e["name"] = p.name;
    e["version"] = p.version;
    e["repository"] = r.name;
    e["size"] = p.size;
    e["flash"] = p.flash;
    e["psram"] = p.psram;
    e["abi"] = p.abi;
    e["depends"] = p.depends;
    e["description"] = p.description;
    const char *installed = "";
    this->for_each([&installed, &p](const Package &i) {
      if (std::strncmp(i.name, p.name, sizeof(i.name)) == 0)
        installed = i.version;
    });
    e["installed"] = installed;
  });
  root["refreshing"] = this->is_refreshing();
  root["installing"] = this->installing_name();
  root["last_error"] = this->last_error();
}
#endif

}

#endif

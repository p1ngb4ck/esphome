#include "api_actions.h"

#if defined(USE_ESP_IDF) && defined(USE_PACKAGE_MANAGER) && defined(USE_PACKAGE_MANAGER_API)

#include "esphome/core/helpers.h"

#include <cstdlib>

namespace esphome::package_manager {

static constexpr ApiActionStrings<0> LIST_STRINGS("package_list", {});
static constexpr ApiActionStrings<5> INSTALL_STRINGS("package_install",
                                                     {"source", "name", "package_version", "abi", "target"});
static constexpr ApiActionStrings<1> ENABLE_STRINGS("package_enable", {"name"});
static constexpr ApiActionStrings<1> DISABLE_STRINGS("package_disable", {"name"});
static constexpr ApiActionStrings<1> REMOVE_STRINGS("package_remove", {"name"});
static constexpr ApiActionStrings<2> RECOVERY_STRINGS("package_recovery", {"name", "recovery"});
static constexpr ApiActionStrings<2> HOLD_STRINGS("package_hold", {"name", "hold"});
static constexpr ApiActionStrings<1> INFO_STRINGS("package_info", {"name"});
static constexpr ApiActionStrings<1> FIND_STRINGS("package_find", {"query"});
static constexpr ApiActionStrings<1> FILES_STRINGS("package_files", {"name"});
static constexpr ApiActionStrings<1> DEPENDS_STRINGS("package_depends", {"name"});
static constexpr ApiActionStrings<1> WHATDEPENDS_STRINGS("package_whatdepends", {"name"});
static constexpr ApiActionStrings<1> WHATDEPENDSREC_STRINGS("package_whatdependsrec", {"name"});
static constexpr ApiActionStrings<0> ARCH_STRINGS("package_architecture", {});
#ifdef USE_PACKAGE_MANAGER_HTTP
static constexpr ApiActionStrings<0> AVAILABLE_STRINGS("package_available", {});
static constexpr ApiActionStrings<0> REFRESH_STRINGS("package_refresh", {});
static constexpr ApiActionStrings<3> REPO_ADD_STRINGS("package_repo_add", {"name", "url", "public_key"});
static constexpr ApiActionStrings<1> REPO_DELETE_STRINGS("package_repo_delete", {"name"});
static constexpr ApiActionStrings<2> REPO_ENABLE_STRINGS("package_repo_enable", {"name", "enabled"});
static constexpr ApiActionStrings<2> DOWNLOAD_STRINGS("package_download", {"name", "folder"});
#endif

void PackageManager::register_api_actions() {
  api::APIServer *server = api::global_api_server;
  server->register_user_service(
      new PackageDataAction(LIST_STRINGS.table.data(), fnv1_hash("package_list"), this,
                            +[](PackageManager &pm, JsonObject root) { pm.dump_installed(root); }));
  server->register_user_service(new PackageStatusAction<std::string, std::string, std::string, std::string, std::string>(
      INSTALL_STRINGS.table.data(), fnv1_hash("package_install"), this,
      +[](PackageManager &pm, std::string source, std::string name, std::string version, std::string abi,
          std::string target) {
#ifdef USE_PACKAGE_MANAGER_HTTP
        if (source.empty())
          return pm.install_from_repo(name);
#endif
        return pm.install(source, name, version, static_cast<uint32_t>(strtoul(abi.c_str(), nullptr, 10)),
                          SettingsTarget::PREFERENCES, target);
      }));
  server->register_user_service(new PackageStatusAction<std::string>(
      ENABLE_STRINGS.table.data(), fnv1_hash("package_enable"), this,
      +[](PackageManager &pm, std::string name) { return pm.set_enabled(name, true); }));
  server->register_user_service(new PackageStatusAction<std::string>(
      DISABLE_STRINGS.table.data(), fnv1_hash("package_disable"), this,
      +[](PackageManager &pm, std::string name) { return pm.set_enabled(name, false); }));
  server->register_user_service(
      new PackageStatusAction<std::string>(REMOVE_STRINGS.table.data(), fnv1_hash("package_remove"), this,
                                           +[](PackageManager &pm, std::string name) { return pm.remove(name); }));
  server->register_user_service(new PackageStatusAction<std::string, bool>(
      RECOVERY_STRINGS.table.data(), fnv1_hash("package_recovery"), this,
      +[](PackageManager &pm, std::string name, bool recovery) { return pm.set_recovery(name, recovery); }));
  server->register_user_service(new PackageStatusAction<std::string, bool>(
      HOLD_STRINGS.table.data(), fnv1_hash("package_hold"), this,
      +[](PackageManager &pm, std::string name, bool hold) { return pm.set_hold(name, hold); }));
  server->register_user_service(new PackageQueryAction(
      INFO_STRINGS.table.data(), fnv1_hash("package_info"), this,
      +[](PackageManager &pm, const std::string &name, JsonObject root) { pm.dump_info(name, root); }));
  server->register_user_service(new PackageQueryAction(
      FILES_STRINGS.table.data(), fnv1_hash("package_files"), this,
      +[](PackageManager &pm, const std::string &name, JsonObject root) { pm.dump_files(name, root); }));
  server->register_user_service(new PackageQueryAction(
      FIND_STRINGS.table.data(), fnv1_hash("package_find"), this,
      +[](PackageManager &pm, const std::string &query, JsonObject root) { pm.dump_find(query, root); }));
  server->register_user_service(new PackageQueryAction(
      DEPENDS_STRINGS.table.data(), fnv1_hash("package_depends"), this,
      +[](PackageManager &pm, const std::string &name, JsonObject root) { pm.dump_depends(name, false, false, root); }));
  server->register_user_service(new PackageQueryAction(
      WHATDEPENDS_STRINGS.table.data(), fnv1_hash("package_whatdepends"), this,
      +[](PackageManager &pm, const std::string &name, JsonObject root) { pm.dump_depends(name, true, false, root); }));
  server->register_user_service(new PackageQueryAction(
      WHATDEPENDSREC_STRINGS.table.data(), fnv1_hash("package_whatdependsrec"), this,
      +[](PackageManager &pm, const std::string &name, JsonObject root) { pm.dump_depends(name, true, true, root); }));
  server->register_user_service(
      new PackageDataAction(ARCH_STRINGS.table.data(), fnv1_hash("package_architecture"), this,
                            +[](PackageManager &pm, JsonObject root) { pm.dump_architecture(root); }));
#ifdef USE_PACKAGE_MANAGER_HTTP
  server->register_user_service(
      new PackageDataAction(AVAILABLE_STRINGS.table.data(), fnv1_hash("package_available"), this,
                            +[](PackageManager &pm, JsonObject root) { pm.dump_available(root); }));
  server->register_user_service(new PackageStatusAction<>(REFRESH_STRINGS.table.data(), fnv1_hash("package_refresh"),
                                                          this, +[](PackageManager &pm) { return pm.refresh(); }));
  server->register_user_service(new PackageStatusAction<std::string, std::string, std::string>(
      REPO_ADD_STRINGS.table.data(), fnv1_hash("package_repo_add"), this,
      +[](PackageManager &pm, std::string name, std::string url, std::string public_key) {
        return pm.repo_add(name, url, public_key);
      }));
  server->register_user_service(new PackageStatusAction<std::string>(
      REPO_DELETE_STRINGS.table.data(), fnv1_hash("package_repo_delete"), this,
      +[](PackageManager &pm, std::string name) { return pm.repo_delete(name); }));
  server->register_user_service(new PackageStatusAction<std::string, bool>(
      REPO_ENABLE_STRINGS.table.data(), fnv1_hash("package_repo_enable"), this,
      +[](PackageManager &pm, std::string name, bool enabled) { return pm.repo_enable(name, enabled); }));
  server->register_user_service(new PackageStatusAction<std::string, std::string>(
      DOWNLOAD_STRINGS.table.data(), fnv1_hash("package_download"), this,
      +[](PackageManager &pm, std::string name, std::string folder) { return pm.download(name, folder); }));
#endif
}

}

#endif

#include "package_manager.h"

#if defined(USE_ESP_IDF) && defined(USE_PACKAGE_MANAGER) && defined(USE_PACKAGE_MANAGER_MQTT)

#include "esphome/components/mqtt/mqtt_client.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace esphome::package_manager {

void PackageManager::setup_mqtt_() {
  mqtt::MQTTClientComponent *mqtt = mqtt::global_mqtt_client;
  mqtt->subscribe_json(mqtt->get_topic_prefix() + "/package/cmd",
                       [this](const std::string &topic, JsonObject root) { this->mqtt_command_(root); });
  mqtt->set_on_connect([this](bool session_present) { this->defer([this]() { this->mqtt_state_(); }); });
  this->add_on_change_callback([this]() { this->mqtt_state_(); });
}

void PackageManager::mqtt_state_() {
  mqtt::MQTTClientComponent *mqtt = mqtt::global_mqtt_client;
  if (!mqtt->is_connected())
    return;
  mqtt->publish_json(mqtt->get_topic_prefix() + "/package/state",
                     [this](JsonObject root) { this->dump_installed(root); }, 0, true);
}

void PackageManager::mqtt_command_(JsonObject root) {
  const std::string op = root["op"] | "";
  const std::string name = root["name"] | "";
  const std::string query = root["query"] | "";
  bool ok = false;
  bool data = false;
  if (op == "list") {
    ok = data = true;
  } else if (op == "install") {
    const std::string source = root["source"] | "";
#ifdef USE_PACKAGE_MANAGER_HTTP
    if (source.empty()) {
      ok = this->install_from_repo(name);
    } else
#endif
    {
      ok = this->install(source, name, root["version"] | "", root["abi"] | 0u, SettingsTarget::PREFERENCES,
                         root["target"] | "");
    }
  } else if (op == "enable") {
    ok = this->set_enabled(name, true);
  } else if (op == "disable") {
    ok = this->set_enabled(name, false);
  } else if (op == "remove") {
    ok = this->remove(name);
  } else if (op == "recovery") {
    ok = this->set_recovery(name, root["recovery"] | true);
  } else if (op == "hold" || op == "unhold") {
    ok = this->set_hold(name, op == "hold");
  } else if (op == "info" || op == "files" || op == "find" || op == "depends" || op == "whatdepends" ||
             op == "whatdependsrec" || op == "architecture") {
    ok = data = true;
#ifdef USE_PACKAGE_MANAGER_HTTP
  } else if (op == "available") {
    ok = data = true;
  } else if (op == "refresh") {
    ok = this->refresh();
  } else if (op == "repo_add") {
    ok = this->repo_add(name, root["url"] | "", root["public_key"] | "");
  } else if (op == "repo_delete") {
    ok = this->repo_delete(name);
  } else if (op == "repo_enable") {
    ok = this->repo_enable(name, root["enabled"] | true);
  } else if (op == "download") {
    ok = this->download(name, root["folder"] | "");
#endif
  } else {
    this->refuse_("Unknown package command '%s'", op.c_str());
  }
  mqtt::MQTTClientComponent *mqtt = mqtt::global_mqtt_client;
  mqtt->publish_json(mqtt->get_topic_prefix() + "/package/result", [this, &op, &name, &query, ok, data](JsonObject out) {
    out["op"] = op;
    out["name"] = name;
    out["success"] = ok;
    out["error"] = ok ? "" : this->last_error();
    if (!data)
      return;
    if (op == "info") {
      this->dump_info(name, out);
      return;
    }
    if (op == "files") {
      this->dump_files(name, out);
      return;
    }
    if (op == "find") {
      this->dump_find(query, out);
      return;
    }
    if (op == "depends" || op == "whatdepends" || op == "whatdependsrec") {
      this->dump_depends(name, op != "depends", op == "whatdependsrec", out);
      return;
    }
    if (op == "architecture") {
      this->dump_architecture(out);
      return;
    }
#ifdef USE_PACKAGE_MANAGER_HTTP
    if (op == "available") {
      this->dump_available(out);
      return;
    }
#endif
    this->dump_installed(out);
  });
}

}

#endif

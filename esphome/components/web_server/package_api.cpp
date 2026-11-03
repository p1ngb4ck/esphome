#include "package_api.h"

#if defined(USE_WEBSERVER_PACKAGE_API) && defined(USE_ESP_IDF)

#include "esphome/components/json/json_util.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace esphome::web_server {

static const char *const API_PREFIX = "/api/packages/";
static const char *const CARD_URL = "/packages/card.js";

void WebServerPackageApi::setup() { this->base_->add_handler(this); }

void WebServerPackageApi::run_on_loop_(std::function<void()> &&op) {
  TaskHandle_t waiter = xTaskGetCurrentTaskHandle();
  std::atomic<bool> done{false};
  this->defer([op = std::move(op), waiter, &done]() {
    op();
    done.store(true);
    xTaskNotifyGive(waiter);
  });
  while (!done.load())
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

bool WebServerPackageApi::canHandle(AsyncWebServerRequest *request) const {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(url_buf);
  const auto method = request->method();
  if (method == HTTP_GET && url == CARD_URL)
    return true;
  if (strncmp(url.c_str(), API_PREFIX, strlen(API_PREFIX)) != 0)
    return false;
  return method == HTTP_GET || method == HTTP_POST;
}

static std::string param(AsyncWebServerRequest *request, const char *name) {
  auto *p = request->getParam(name);
  return p != nullptr ? std::string(p->value()) : std::string();
}

static bool param_on(AsyncWebServerRequest *request, const char *name) {
  const std::string v = param(request, name);
  return v.empty() || v == "1" || v == "true";
}

bool WebServerPackageApi::command_(const char *op, AsyncWebServerRequest *request) {
  package_manager::PackageManager *pm = this->pm_;
  const std::string name = param(request, "name");
  if (strcmp(op, "install") == 0) {
    const std::string source = param(request, "source");
#ifdef USE_PACKAGE_MANAGER_HTTP
    if (source.empty())
      return pm->install_from_repo(name);
#endif
    return pm->install(source, name, param(request, "version"),
                       static_cast<uint32_t>(strtoul(param(request, "abi").c_str(), nullptr, 10)),
                       package_manager::SettingsTarget::PREFERENCES, param(request, "target"));
  }
  if (strcmp(op, "enable") == 0)
    return pm->set_enabled(name, true);
  if (strcmp(op, "disable") == 0)
    return pm->set_enabled(name, false);
  if (strcmp(op, "remove") == 0)
    return pm->remove(name);
  if (strcmp(op, "recovery") == 0)
    return pm->set_recovery(name, param_on(request, "recovery"));
  if (strcmp(op, "hold") == 0 || strcmp(op, "unhold") == 0)
    return pm->set_hold(name, strcmp(op, "hold") == 0);
#ifdef USE_PACKAGE_MANAGER_HTTP
  if (strcmp(op, "refresh") == 0)
    return pm->refresh();
  if (strcmp(op, "repo_add") == 0)
    return pm->repo_add(name, param(request, "url"), param(request, "public_key"));
  if (strcmp(op, "repo_delete") == 0)
    return pm->repo_delete(name);
  if (strcmp(op, "repo_enable") == 0)
    return pm->repo_enable(name, param_on(request, "enabled"));
  if (strcmp(op, "download") == 0)
    return pm->download(name, param(request, "folder"));
#endif
  return false;
}

void WebServerPackageApi::handleRequest(AsyncWebServerRequest *request) {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(url_buf);
  const bool is_get = request->method() == HTTP_GET;

  if (is_get && url == CARD_URL) {
    auto *response = request->beginResponse(200, "text/javascript", ESPHOME_WEBSERVER_PACKAGES_JS,
                                            ESPHOME_WEBSERVER_PACKAGES_JS_SIZE);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "public, max-age=3600");
    request->send(response);
    return;
  }

  const char *op = url.c_str() + strlen(API_PREFIX);
  std::string json;
  if (is_get) {
    static const char *const QUERIES[] = {"list",           "info",         "files",    "find", "depends", "whatdepends",
                                          "whatdependsrec", "architecture", "available"};
    bool known = false;
    for (const char *query : QUERIES)
      known = known || strcmp(op, query) == 0;
#ifndef USE_PACKAGE_MANAGER_HTTP
    known = known && strcmp(op, "available") != 0;
#endif
    if (!known) {
      request->send(404, "application/json", "{\"error\":\"unknown request\"}");
      return;
    }
    const std::string name = param(request, "name");
    const std::string query = param(request, "q");
    this->run_on_loop_([this, op, &name, &query, &json]() {
      json::JsonBuilder builder;
      JsonObject root = builder.root();
      if (strcmp(op, "info") == 0) {
        this->pm_->dump_info(name, root);
      } else if (strcmp(op, "files") == 0) {
        this->pm_->dump_files(name, root);
      } else if (strcmp(op, "find") == 0) {
        this->pm_->dump_find(query, root);
      } else if (strcmp(op, "depends") == 0 || strcmp(op, "whatdepends") == 0 || strcmp(op, "whatdependsrec") == 0) {
        this->pm_->dump_depends(name, strcmp(op, "depends") != 0, strcmp(op, "whatdependsrec") == 0, root);
      } else if (strcmp(op, "architecture") == 0) {
        this->pm_->dump_architecture(root);
#ifdef USE_PACKAGE_MANAGER_HTTP
      } else if (strcmp(op, "available") == 0) {
        this->pm_->dump_available(root);
#endif
      } else {
        this->pm_->dump_installed(root);
      }
      auto buf = builder.serialize();
      json.assign(buf.data(), buf.size());
    });
    request->send(200, "application/json", json.c_str());
    return;
  }

  this->run_on_loop_([this, op, request, &json]() {
    const bool ok = this->command_(op, request);
    json::JsonBuilder builder;
    JsonObject root = builder.root();
    root["success"] = ok;
    root["error"] = ok ? "" : (this->pm_->last_error()[0] != '\0' ? this->pm_->last_error() : "unknown request");
    auto buf = builder.serialize();
    json.assign(buf.data(), buf.size());
  });
  request->send(200, "application/json", json.c_str());
}

}

#endif

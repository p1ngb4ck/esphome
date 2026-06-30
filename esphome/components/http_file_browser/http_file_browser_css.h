#pragma once

static constexpr const char HTTP_FILE_BROWSER_CSS[] = R"CSS(
body { font-family: 'Segoe UI', system-ui, sans-serif; margin: 0; padding: 2rem; background: #f5f5f7; color: #1d1d1f; }
h1 { color: #0066cc; margin-bottom: 1.5rem; display: flex; align-items: center; gap: 1rem; }
.container { max-width: 1200px; margin: 0 auto; background: white; border-radius: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.1); padding: 2rem; }
table { width: 100%; border-collapse: collapse; margin-top: 1.5rem; }
th, td { padding: 12px; text-align: left; border-bottom: 1px solid #e0e0e0; }
th { background: #f8f9fa; font-weight: 500; }
.file-actions { display: flex; gap: 8px; }
button { padding: 6px 12px; border: none; border-radius: 6px; background: #0066cc; color: white; cursor: pointer; transition: background 0.2s; }
button:hover { background: #0052a3; }
button.delete { background: #dc3545; }
button.delete:hover { background: #c82333; }
.upload-form { margin-bottom: 2rem; padding: 1rem; background: #f8f9fa; border-radius: 8px; }
.upload-form input[type="file"] { margin-right: 1rem; }
.breadcrumb { margin-bottom: 1.5rem; font-size: 0.9rem; color: #666; }
.breadcrumb a { color: #0066cc; text-decoration: none; }
.breadcrumb a:hover { text-decoration: underline; }
.breadcrumb span:not(:last-child)::after { display: inline-block; margin: 0 .25rem; content: ">"; }
.folder { color: #0066cc; font-weight: 500; }
.file-type { color: #666; font-size: 0.9rem; }
.header-actions { display: flex; align-items: center; justify-content: space-between; margin-bottom: 1rem; }
.header-actions button { background: #4CAF50; }
.header-actions button:hover { background: #45a049; }
.progress-modal { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.5); z-index: 1000; align-items: center; justify-content: center; }
.progress-modal.active { display: flex; }
.progress-content { background: white; padding: 2rem; border-radius: 12px; min-width: 400px; box-shadow: 0 8px 24px rgba(0,0,0,0.2); }
.progress-title { font-size: 1.2rem; font-weight: 500; margin-bottom: 1rem; color: #1d1d1f; }
.progress-bar-container { width: 100%; height: 20px; background: #e0e0e0; border-radius: 10px; overflow: hidden; margin: 1rem 0; }
.progress-bar { height: 100%; background: linear-gradient(90deg, #0066cc, #0052a3); width: 0%; transition: width 0.3s ease; display: flex; align-items: center; justify-content: center; color: white; font-size: 0.8rem; font-weight: 500; }
.progress-details { color: #666; font-size: 0.9rem; margin-top: 0.5rem; }
.progress-file-info { margin-top: 1rem; padding: 1rem; background: #f8f9fa; border-radius: 8px; font-size: 0.85rem; word-break: break-all; }
.progress-speed { margin-top: 0.5rem; font-size: 0.9rem; color: #666; }
#cancelBtn { margin-top: 1rem; width: 100%; }
)CSS";

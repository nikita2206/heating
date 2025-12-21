/**
 * OpenTherm Gateway - SPA Entry Point
 */

import './styles/main.css';
import { renderNav } from './components/Nav.js';
import { renderDashboard, initDashboard, destroyDashboard } from './pages/Dashboard.js';
import { renderLogs, initLogs, destroyLogs } from './pages/Logs.js';
import { renderDiagnostics, initDiagnostics, destroyDiagnostics } from './pages/Diagnostics.js';
import { renderMqtt, initMqtt, destroyMqtt } from './pages/Mqtt.js';
import { renderWrite, initWrite, destroyWrite } from './pages/Write.js';
import { renderOta, initOta, destroyOta } from './pages/Ota.js';

const routes = {
  '/': { render: renderDashboard, init: initDashboard, destroy: destroyDashboard },
  '/logs': { render: renderLogs, init: initLogs, destroy: destroyLogs },
  '/diagnostics': { render: renderDiagnostics, init: initDiagnostics, destroy: destroyDiagnostics },
  '/mqtt': { render: renderMqtt, init: initMqtt, destroy: destroyMqtt },
  '/write': { render: renderWrite, init: initWrite, destroy: destroyWrite },
  '/ota': { render: renderOta, init: initOta, destroy: destroyOta }
};

let currentRoute = null;
let currentDestroy = null;

function getPath() {
  const hash = window.location.hash || '#/';
  return hash.slice(1) || '/';
}

function navigate(path) {
  // Clean up previous page
  if (currentDestroy) {
    currentDestroy();
    currentDestroy = null;
  }

  const route = routes[path] || routes['/'];
  currentRoute = path;

  const app = document.getElementById('app');
  app.innerHTML = renderNav(path) + route.render();

  // Initialize the new page
  if (route.init) {
    route.init();
    currentDestroy = route.destroy || null;
  }
}

function handleNavigation() {
  const path = getPath();
  if (path !== currentRoute) {
    navigate(path);
  }
}

// Initialize router
window.addEventListener('hashchange', handleNavigation);
window.addEventListener('load', handleNavigation);

// Handle initial load
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', handleNavigation);
} else {
  handleNavigation();
}

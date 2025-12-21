/**
 * Navigation component
 */

const NAV_ITEMS = [
  { path: '/', label: 'Dashboard' },
  { path: '/logs', label: 'Logs' },
  { path: '/diagnostics', label: 'Diagnostics' },
  { path: '/mqtt', label: 'MQTT' },
  { path: '/write', label: 'Manual Write' },
  { path: '/ota', label: 'OTA Update' }
];

export function renderNav(currentPath) {
  const navLinks = NAV_ITEMS.map(item => {
    const isActive = item.path === currentPath;
    return `<a href="#${item.path}" class="${isActive ? 'active' : ''}">${item.label}</a>`;
  }).join('');

  return `
    <nav>
      <a href="#/" class="logo">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
          <path d="M12 2L2 7l10 5 10-5-10-5zM2 17l10 5 10-5M2 12l10 5 10-5"/>
        </svg>
        OT Gateway
      </a>
      <div class="nav-links">
        ${navLinks}
      </div>
    </nav>
  `;
}

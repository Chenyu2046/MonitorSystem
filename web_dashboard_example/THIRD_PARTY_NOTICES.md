# Third-party notices

Source: https://github.com/superlxh02/linux_monitor_system
Revision: 6f5099e8743521e08d380fb68d9ada8ba41982ec
Licensed under Apache License, Version 2.0 (see LICENSE.upstream).

Copied from upstream `web_dashboard_example/`:
- `public/app.js`, `public/index.html`, `public/styles.css`
- `server.js`, `package.json`, `package-lock.json`

Modifications for KernScope:
- `public/app.js`: resource/health score handling, CPU host-level view, AI drawer mount.
- `public/index.html`: AI script import.
- `server.js`: existing KernScope proto, scoring/metric unit adjustment, Agent API.
- New code: `public/agent.js`, `agent.js`.

Keep this notice and the upstream license when redistributing the adapted Dashboard.

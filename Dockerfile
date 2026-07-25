# Salume Studio — nginx serving the app, plus a small Python recorder/API backend
# running alongside it in the same container (see docker-entrypoint.sh). This is a
# single-container image on purpose: the install script does one `docker build` +
# `docker run`, so everything ships together and persists under the one data
# volume it already mounts.
FROM nginx:1.27-alpine

# Python for the recorder/API backend. Installed into a venv so pip doesn't fight
# Alpine's externally-managed environment; the deps are all pure-Python wheels,
# so no build toolchain is needed.
RUN apk add --no-cache python3 py3-pip \
    && python3 -m venv /opt/venv
COPY backend/requirements.txt /app/requirements.txt
RUN /opt/venv/bin/pip install --no-cache-dir -r /app/requirements.txt
COPY backend/monitor.py /app/monitor.py

# Site config (static serving + /api/ backend proxy + MQTT WebSocket proxy).
COPY nginx/default.conf /etc/nginx/conf.d/default.conf

# The app itself, plus the favicon / app icon and the PWA shell files.
COPY charcuterie.html      /usr/share/nginx/html/charcuterie.html
COPY icon.png              /usr/share/nginx/html/icon.png
COPY manifest.webmanifest  /usr/share/nginx/html/manifest.webmanifest
COPY sw.js                 /usr/share/nginx/html/sw.js

COPY docker-entrypoint.sh /docker-entrypoint-salume.sh
RUN chmod +x /docker-entrypoint-salume.sh

EXPOSE 80

# Healthcheck hits the app shell AND the backend, so the container is only
# "healthy" when both halves are up.
HEALTHCHECK --interval=30s --timeout=3s --retries=3 \
    CMD wget -qO- http://localhost/ >/dev/null 2>&1 \
        && wget -qO- http://localhost/api/health >/dev/null 2>&1 || exit 1

# Override nginx:alpine's default CMD: our entrypoint launches both processes.
ENTRYPOINT ["/docker-entrypoint-salume.sh"]

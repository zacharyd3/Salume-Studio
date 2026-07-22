# Salume Studio — served as a static single-file app behind NGINX.
FROM nginx:1.27-alpine

# Site config (static serving + optional MQTT WebSocket proxy).
COPY nginx/default.conf /etc/nginx/conf.d/default.conf

# The app itself, plus the favicon / app icon.
COPY charcuterie.html /usr/share/nginx/html/charcuterie.html
COPY icon.png         /usr/share/nginx/html/icon.png

EXPOSE 80

# nginx:alpine already sets CMD ["nginx", "-g", "daemon off;"] and ships a
# HEALTHCHECK-free image; add a lightweight healthcheck of our own.
HEALTHCHECK --interval=30s --timeout=3s --retries=3 \
    CMD wget -qO- http://localhost/ >/dev/null 2>&1 || exit 1

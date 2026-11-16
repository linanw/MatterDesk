FROM node:24.22.3-bookworm-slim@sha256:dc26ae92c9fb15cf6b1d63e20f0944b2efe0f115b20e00be48fb23fceb061c54

ENV NODE_ENV=production
WORKDIR /opt/matterdesk/controller
COPY controller/package.json controller/package-lock.json ./
RUN npm ci --omit=dev --no-fund --no-audit && npm cache clean --force \
    && install -d -o node -g node -m 0700 /var/lib/matterdesk
COPY controller/*.mjs ./
COPY deploy/controller-entrypoint.sh /usr/local/bin/matterdesk-controller
RUN chmod 0755 /usr/local/bin/matterdesk-controller

USER node
ENTRYPOINT ["/usr/local/bin/matterdesk-controller"]

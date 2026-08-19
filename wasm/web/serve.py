#!/usr/bin/env python3
"""Serveur statique : isolation cross-origin + service precompresse.

DEUX CHOSES, ET AUCUNE N'EST COSMETIQUE.

1. COOP/COEP. Sans ces deux en-tetes, pas de SharedArrayBuffer — donc pas de
   pthreads, donc UN worker au lieu de seize. C'est LA contrainte de deploiement
   du portage, et elle exclut d'emblee les hebergeurs qui ne posent pas
   d'en-tetes (GitHub Pages). Cloudflare Pages, Netlify et Vercel les acceptent
   via un fichier `_headers`.

2. Brotli precompresse. Les 22 302 scripts Lua se ressemblent enormement ; la
   fenetre de brotli les exploite bien mieux que gzip : 10,56 Mo -> 4,45 Mo sur
   le seul `.data`. Total servi : 12,18 Mo en gzip contre 5,56 Mo en brotli.
   On sert le `.br` deja sur disque quand le client l'accepte — jamais de
   compression a la volee, qui couterait plus qu'elle ne rapporte.
"""
import http.server
import os
import socketserver
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8765


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      '.wasm': 'application/wasm',
                      '.js': 'text/javascript',
                      '.data': 'application/octet-stream'}

    def send_head(self):
        path = self.translate_path(self.path)
        br = path + '.br'
        accepts = 'br' in self.headers.get('Accept-Encoding', '')
        if accepts and os.path.isfile(br) and os.path.isfile(path):
            ctype = self.guess_type(path)
            st = os.stat(br)
            f = open(br, 'rb')
            self.send_response(200)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Encoding', 'br')
            self.send_header('Content-Length', str(st.st_size))
            self.send_header('Vary', 'Accept-Encoding')
            self.end_headers()
            return f
        return super().send_head()

    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Resource-Policy', 'cross-origin')
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def log_message(self, *a):
        pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


with Server(('127.0.0.1', PORT), Handler) as httpd:
    print(f'http://127.0.0.1:{PORT}/  (COOP/COEP + brotli precompresse)', flush=True)
    httpd.serve_forever()

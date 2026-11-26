import http.server, sys, os
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        if self.path.startswith("/csp-site/"):
            self.send_header("Content-Security-Policy", "require-trusted-types-for 'script'; style-src 'self'; script-src 'self'")
        super().end_headers()
os.chdir(sys.argv[1]); http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[2])), H).serve_forever()

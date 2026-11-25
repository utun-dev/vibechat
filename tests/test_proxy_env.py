#!/usr/bin/env python3
"""Проверки общего proxy.sh и реального скачивания через локальный HTTP-прокси."""
from pathlib import Path
import base64
import contextlib
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import io
import os
import shutil
import subprocess
import tempfile
import threading
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('proxy_dependencies', ROOT / 'tools/prepare_dependencies.py')
DEPS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DEPS)
BODY = b'proxied dependency fixture'
PROXY_VARIABLES = ('http_proxy', 'https_proxy', 'HTTP_PROXY', 'HTTPS_PROXY', 'all_proxy', 'ALL_PROXY', 'no_proxy', 'NO_PROXY')


class ProxyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='utun-proxy-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.environment = mock.patch.dict(os.environ)
        self.environment.start()
        self.addCleanup(self.environment.stop)
        for key in PROXY_VARIABLES:
            os.environ.pop(key, None)

    def configure(self, url):
        template = (ROOT / 'proxy.sh.example').read_text()
        (self.root / 'proxy.sh').write_text(template.replace("UTUN_HTTP_PROXY=''", f"UTUN_HTTP_PROXY='{url}'"))

    def test_empty_profile_preserves_environment(self):
        self.configure('')
        os.environ['https_proxy'] = 'http://inherited.invalid:8080'
        DEPS.load_proxy(self.root)
        self.assertEqual(os.environ['https_proxy'], 'http://inherited.invalid:8080')

    def test_bad_profile_is_rejected_without_logging_credentials(self):
        for content in ["UTUN_HTTP_PROXY='socks5://user:secret@host:1080'", "UTUN_HTTP_PROXY=$UNKNOWN"]:
            (self.root / 'proxy.sh').write_text(content)
            with self.assertRaises(RuntimeError) as error:
                DEPS.load_proxy(self.root)
            self.assertNotIn('secret', str(error.exception))

    @unittest.skipUnless(shutil.which('curl'), 'curl is required')
    def test_shell_and_python_downloads_use_real_proxy(self):
        requests = []

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                requests.append((self.path, self.headers.get('Proxy-Authorization')))
                self.send_response(200)
                self.send_header('Content-Length', str(len(BODY)))
                self.end_headers()
                self.wfile.write(BODY)

            def log_message(self, *args):
                pass

        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.configure(f'http://user:secret@127.0.0.1:{server.server_port}')
        url = 'http://source.invalid/dependency'
        for shell in ('sh', 'bash'):
            result = subprocess.run([shell, '-c', '. "$1"; exec curl --fail --silent --show-error --max-time 5 "$2"',
                                     shell, str(self.root / 'proxy.sh'), url], capture_output=True, check=True)
            self.assertEqual(result.stdout, BODY)
        output = io.StringIO()
        (self.root / 'cache').mkdir()
        with contextlib.redirect_stdout(output):
            DEPS.load_proxy(self.root)
            with mock.patch.dict(DEPS.DOWNLOADS, {'fixture': (url, hashlib.sha256(BODY).hexdigest())}):
                downloaded = DEPS.download('fixture', self.root / 'cache', False)
        self.assertEqual(downloaded.read_bytes(), BODY)
        self.assertNotIn('secret', output.getvalue())
        self.assertEqual(len(requests), 3)
        authorization = 'Basic ' + base64.b64encode(b'user:secret').decode()
        self.assertTrue(all(path == url and auth == authorization for path, auth in requests))


if __name__ == '__main__':
    unittest.main()

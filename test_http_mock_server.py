#!/usr/bin/env python3
"""
Mock HTTP Server for Testing
Returns various status codes, delays, and response sizes for end-to-end testing
"""

import http.server
import socketserver
import json
import time
import sys
from urllib.parse import urlparse, parse_qs

class MockHTTPHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        query = parse_qs(parsed.query)
        
        # Default response
        status_code = 200
        delay = 0
        size = 100
        content_type = 'application/json'
        
        # Parse path for status code
        if path.startswith('/status/'):
            try:
                status_code = int(path.split('/status/')[1].split('/')[0])
            except (ValueError, IndexError):
                status_code = 200
        
        # Parse query parameters
        if 'delay' in query:
            try:
                delay = float(query['delay'][0])
            except (ValueError, IndexError):
                delay = 0
        
        if 'size' in query:
            try:
                size = int(query['size'][0])
            except (ValueError, IndexError):
                size = 100
        
        if 'type' in query:
            content_type = query['type'][0]
        
        # Apply delay
        if delay > 0:
            time.sleep(delay)
        
        # Generate response body
        if content_type == 'application/json':
            response_data = {
                'status': status_code,
                'path': path,
                'delay': delay,
                'size': size,
                'message': 'Mock response',
                'data': 'x' * max(0, size - 100)  # Fill remaining size
            }
            body = json.dumps(response_data).encode('utf-8')
            # Adjust to exact size if needed
            if len(body) < size:
                body += b' ' * (size - len(body))
            elif len(body) > size:
                body = body[:size]
        else:
            body = ('x' * size).encode('utf-8')
        
        # Send response
        self.send_response(status_code)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('X-Mock-Server', 'true')
        self.end_headers()
        self.wfile.write(body)
    
    def do_POST(self):
        # Read request body
        content_length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_length) if content_length > 0 else b''
        
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)
        
        status_code = 201
        delay = 0
        
        if 'status' in query:
            try:
                status_code = int(query['status'][0])
            except (ValueError, IndexError):
                status_code = 201
        
        if 'delay' in query:
            try:
                delay = float(query['delay'][0])
            except (ValueError, IndexError):
                delay = 0
        
        if delay > 0:
            time.sleep(delay)
        
        response_data = {
            'status': status_code,
            'received': len(body),
            'message': 'POST received'
        }
        response_body = json.dumps(response_data).encode('utf-8')
        
        self.send_response(status_code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(response_body)))
        self.send_header('X-Mock-Server', 'true')
        self.end_headers()
        self.wfile.write(response_body)
    
    def log_message(self, format, *args):
        # Suppress default logging
        pass

def run_server(port=8888):
    with socketserver.TCPServer(("0.0.0.0", port), MockHTTPHandler) as httpd:
        print(f"Mock HTTP server running on port {port}", file=sys.stderr)
        print(f"Ready to accept requests", file=sys.stderr)
        sys.stderr.flush()
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nShutting down server", file=sys.stderr)
            httpd.shutdown()

if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8888
    run_server(port)


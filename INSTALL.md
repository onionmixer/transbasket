# Transbasket Installation Guide

## Quick Start

### 1. Prerequisites

- Python 3.8 or higher
- pip (Python package manager)
- Access to OpenAI-compatible API endpoint

### 2. Install Python POC

```bash
# Navigate to POC directory
cd transbasket/poc

# Create virtual environment
python3 -m venv venv

# Activate virtual environment
source venv/bin/activate  # Linux/Mac
# or
venv\Scripts\activate     # Windows

# Install dependencies
pip install -r requirements.txt
```

### 3. Configure

Ensure `transbasket.conf` exists in the parent directory with valid settings:

```bash
cd transbasket
cat transbasket.conf
```

Expected content:
```bash
OPENAI_BASE_URL="http://192.168.1.239:11434/v1"
OPENAI_MODEL="gpt-oss:20b"
OPENAI_API_KEY="."
LISTEN="0.0.0.0"
PORT="8889"
PROMPT_PREFIX="translation from language to language. not add more text. note add emoji syntax. only return plain translate text."
```

### 4. Run Server

```bash
cd poc
python main.py
```

The server will start on the configured address (default: http://0.0.0.0:8889)

### 5. Test

In a separate terminal:

```bash
cd poc
python test_client.py
```

Or test with curl:

```bash
curl http://localhost:8889/health
```

## Project Structure

```
transbasket/
├── README.md                # Project documentation
├── TODO.md                  # Development roadmap
├── INSTALL.md              # This file
├── transbasket.conf        # Configuration file
├── sample_send.txt         # Prompt template
│
└── poc/                    # Python POC
    ├── __init__.py         # Package initialization
    ├── main.py             # Main entry point
    ├── config_loader.py    # Configuration loader
    ├── utils.py            # Utility functions
    ├── json_handler.py     # JSON request/response handler
    ├── openai_client.py    # OpenAI API client
    ├── http_server.py      # FastAPI HTTP server
    ├── test_client.py      # Test client
    ├── requirements.txt    # Python dependencies
    └── README.md          # POC documentation
```

## Module Overview

### 1. config_loader.py (2. 설정파일 로드)
- Loads and validates `transbasket.conf`
- Parses configuration in KEY="value" format
- Validates required fields and formats

### 2. json_handler.py (3. client 요청 JSON 처리)
- Parses client JSON requests
- Validates RFC 3339 timestamps
- Validates UUID v4 format
- Validates ISO 639-2 language codes
- Generates JSON responses

### 3. openai_client.py (4. openai api 서버 JSON 처리)
- Builds prompts from template
- Sends requests to OpenAI API
- Handles retries and timeouts
- Processes API responses

### 4. utils.py (5. 공통 유틸리티 함수)
- UUID validation
- Timestamp validation (RFC 3339)
- Language code validation (ISO 639-2)
- Text truncation
- Helper functions

### 5. http_server.py (6. http 처리 network 루틴)
- FastAPI-based HTTP server
- Async request handling
- Thread pool for blocking operations
- Error handling and responses

### 6. main.py (1. 메인루틴)
- Entry point
- Logging setup
- Signal handling
- Server lifecycle management

## Running Options

### Development Mode (with auto-reload)

```bash
cd poc
uvicorn main:app --reload --host 0.0.0.0 --port 8889
```

### Production Mode

```bash
cd poc
python main.py
```

### With Custom Settings

```bash
# Custom log level
LOG_LEVEL=DEBUG python main.py

# Custom worker count
MAX_WORKERS=20 python main.py

# Custom config path
TRANSBASKET_CONFIG=/path/to/config.conf python main.py
```

## Systemd Service (Optional)

Create `/etc/systemd/system/transbasket.service`:

```ini
[Unit]
Description=Transbasket Translation Service
After=network.target

[Service]
Type=simple
User=transbasket
Group=transbasket
WorkingDirectory=/opt/transbasket/poc
Environment="PATH=/opt/transbasket/poc/venv/bin"
ExecStart=/opt/transbasket/poc/venv/bin/python main.py
Restart=on-failure
RestartSec=5s

# Logging
StandardOutput=journal
StandardError=journal
SyslogIdentifier=transbasket

[Install]
WantedBy=multi-user.target
```

Enable and start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable transbasket
sudo systemctl start transbasket
sudo systemctl status transbasket
```

View logs:

```bash
sudo journalctl -u transbasket -f
```

## Troubleshooting

### Import Errors

Make sure you're in the POC directory and virtual environment is activated:

```bash
cd poc
source venv/bin/activate
which python  # Should point to venv/bin/python
```

### Configuration Not Found

Check if configuration file exists:

```bash
ls -l ../transbasket.conf
```

Or specify custom path:

```bash
TRANSBASKET_CONFIG=/path/to/transbasket.conf python main.py
```

### Port Already in Use

Change port in `transbasket.conf` or kill the process using the port:

```bash
# Find process
lsof -i :8889

# Kill process
kill -9 <PID>
```

### OpenAI API Connection Failed

Test API endpoint directly:

```bash
curl -X POST http://192.168.1.239:11434/v1/chat/completions \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer ." \
  -d '{
    "model": "gpt-oss:20b",
    "messages": [{"role": "user", "content": "Hello"}]
  }'
```

## Next Steps

1. Review `poc/README.md` for detailed API documentation
2. Review `TODO.md` for development roadmap
3. Run test suite: `python test_client.py`
4. Configure systemd service for production
5. Set up monitoring and logging

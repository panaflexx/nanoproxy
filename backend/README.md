# Nanoserver Feed Backend

Python + FastAPI + SQLite backend for the social feed demo.

## Setup

```bash
cd backend
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Run

```bash
uvicorn app:app --reload
```

The API will be available at `http://localhost:8000`.

The first request will automatically create `feed.db` and seed demo data.

## Endpoints

- `GET /api/posts?page=1&count=10` — paginated posts (newest first)

Each post includes:
- user, timestamp, type, text/quote, media[], comments[], likes count

## Frontend integration

Update `public/script.js` to call this API instead of the mock `fetchPosts` function.
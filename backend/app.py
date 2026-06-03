"""FastAPI backend with fastapi-login (equivalent to Flask-Login).

Demo backend for a simple social media app

Features:
- User registration / login (JWT via cookie or Bearer token)
- Protected feed API
- SQLite with password_hash support
"""

import json
import sqlite3
from contextlib import asynccontextmanager
from datetime import datetime, timedelta
from pathlib import Path
from typing import Optional
from uuid import uuid4

import bcrypt
from fastapi import (
    Depends,
    FastAPI,
    File,
    HTTPException,
    Query,
    Response,
    UploadFile,
    status,
)
from fastapi.middleware.cors import CORSMiddleware
from fastapi.security import OAuth2PasswordBearer, OAuth2PasswordRequestForm
from fastapi.staticfiles import StaticFiles
from fastapi_login import LoginManager
from fastapi_login.exceptions import InvalidCredentialsException
from pydantic import BaseModel

# ---------- Configuration ----------

DB_PATH = Path(__file__).parent / "feed.db"
SCHEMA_PATH = Path(__file__).parent / "schema.sql"

# Media storage
USERMEDIA_DIR = Path(__file__).parent / "usermedia"
USERMEDIA_DIR.mkdir(exist_ok=True)

SECRET = "change-this-to-a-very-long-random-secret-in-production"
ALGORITHM = "HS256"
ACCESS_TOKEN_EXPIRE_MINUTES = 60 * 24  # 24 hours


@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db()
    yield


app = FastAPI(title="Nanoserver Feed API (FastAPI + fastapi-login)", lifespan=lifespan)

# Serve the usermedia folder statically so the browser can load the files
app.mount("/usermedia", StaticFiles(directory=str(USERMEDIA_DIR)), name="usermedia")


class RequestDebugMiddleware:
    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        if scope["type"] == "http":
            method = scope.get("method", "?")
            path = scope.get("path", "?")
            headers = {
                k.decode("latin-1", "ignore"): v.decode("latin-1", "ignore")
                for k, v in scope.get("headers", [])
            }
            # print(f"[BACKEND] {method} {path} headers={headers}")

            # Capture the full body
            body_chunks = []
            more_body = True
            while more_body:
                message = await receive()
                body_chunks.append(message.get("body", b""))
                more_body = message.get("more_body", False)
            body = b"".join(body_chunks)
            # print(f"[BACKEND] body ({len(body)} bytes): {body!r}")

            # Re-inject the captured body so downstream can read it
            # Deliver the body exactly once, then empty messages (standard ASGI pattern)
            body_to_send = body
            sent = False

            async def receive_wrapper():
                nonlocal body_to_send, sent
                if not sent:
                    sent = True
                    return {
                        "type": "http.request",
                        "body": body_to_send,
                        "more_body": False,
                    }
                return {"type": "http.request", "body": b"", "more_body": False}

            await self.app(scope, receive_wrapper, send)
            return
        await self.app(scope, receive, send)


app.add_middleware(RequestDebugMiddleware)

app.add_middleware(
    CORSMiddleware,
    allow_origin_regex=r"http://(localhost|127\.0\.0\.1)(:\d+)?",
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# fastapi-login manager
manager = LoginManager(SECRET, token_url="/api/login", use_cookie=True)
manager.cookie_name = "access_token"


class User(BaseModel):
    id: int
    username: str
    display_name: Optional[str] = None


@manager.user_loader()
def load_user(username: str):
    with get_db() as conn:
        row = conn.execute(
            "SELECT id, username, display_name FROM users WHERE username = ?",
            (username,),
        ).fetchone()
        if row:
            return User(
                id=row["id"], username=row["username"], display_name=row["display_name"]
            )
    return None


def get_db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    return conn


def verify_password(plain_password: str, hashed_password: str) -> bool:
    return bcrypt.checkpw(plain_password.encode(), hashed_password.encode())


def get_password_hash(password: str) -> str:
    # bcrypt has a 72-byte limit; truncate to be safe
    password = password[:72]
    return bcrypt.hashpw(password.encode(), bcrypt.gensalt()).decode()


def init_db():
    db_existed = DB_PATH.exists()
    with get_db() as conn:
        conn.executescript(SCHEMA_PATH.read_text())
        if not db_existed:
            seed_data(conn)
            print(f"Initialized database at {DB_PATH}")
        else:
            # Ensure we have at least the demo users if table is empty
            count = conn.execute("SELECT COUNT(*) FROM users").fetchone()[0]
            if count == 0:
                seed_data(conn)
                print("Seeded demo data into existing database")


def seed_data(conn: sqlite3.Connection):
    users = [
        ("alice", "Alice Chen", "https://i.pravatar.cc/150?img=1"),
        ("bob", "Bob Rivera", "https://i.pravatar.cc/150?img=2"),
        ("carol", "Carol Kim", "https://i.pravatar.cc/150?img=3"),
        ("dave", "Dave Patel", "https://i.pravatar.cc/150?img=4"),
    ]
    for username, display_name, avatar in users:
        pw_hash = get_password_hash("demo")
        conn.execute(
            """INSERT INTO users (username, display_name, avatar_url, password_hash)
               VALUES (?, ?, ?, ?)""",
            (username, display_name, avatar, pw_hash),
        )

    posts = [
        (
            1,
            "text",
            "Just deployed the new nanoserver! 🚀 Loving the performance.",
            None,
        ),
        (2, "single-media", "Check out this sunset from my hike today.", None),
        (
            3,
            "big-text",
            None,
            "The only way to do great work is to love what you do. — Steve Jobs",
        ),
        (1, "multi-media", "Some photos from the weekend trip", None),
        (4, "text", "Anyone else obsessed with the new Zed editor?", None),
    ]
    conn.executemany(
        "INSERT INTO posts (user_id, type, text, quote) VALUES (?, ?, ?, ?)",
        posts,
    )

    media = [
        (2, "image", "https://picsum.photos/id/1018/600/400", 0),
        (4, "image", "https://picsum.photos/id/1005/600/400", 0),
        (4, "image", "https://picsum.photos/id/1016/600/400", 1),
        (
            4,
            "video",
            "https://sample-videos.com/video123/mp4/720/big_buck_bunny_720p_1mb.mp4",
            2,
        ),
    ]
    conn.executemany(
        "INSERT INTO media (post_id, type, url, position) VALUES (?, ?, ?, ?)",
        media,
    )

    comments = [
        (1, 2, "Congrats! How's the new architecture working out?"),
        (1, 3, "Need to check that out."),
        (2, 1, "Gorgeous shot!"),
        (4, 3, "Completely agree. The collab features are insane."),
    ]
    conn.executemany(
        "INSERT INTO comments (post_id, user_id, text) VALUES (?, ?, ?)",
        comments,
    )

    likes = [(1, 2), (1, 3), (2, 1), (3, 1), (3, 2), (4, 3)]
    conn.executemany(
        "INSERT OR IGNORE INTO likes (post_id, user_id) VALUES (?, ?)",
        likes,
    )
    conn.commit()


# ---------- Auth Models ----------


class UserCreate(BaseModel):
    username: str
    password: str
    display_name: Optional[str] = None


class Token(BaseModel):
    access_token: str
    token_type: str


# ---------- Auth Endpoints ----------


@app.post("/api/register", status_code=status.HTTP_201_CREATED)
def register(user: UserCreate):
    with get_db() as conn:
        existing = conn.execute(
            "SELECT id FROM users WHERE username = ?", (user.username,)
        ).fetchone()
        if existing:
            raise HTTPException(status_code=409, detail="Username already taken")

        pw_hash = get_password_hash(user.password)
        conn.execute(
            """INSERT INTO users (username, display_name, password_hash)
               VALUES (?, ?, ?)""",
            (user.username, user.display_name or user.username, pw_hash),
        )
        conn.commit()
    return {"message": "User created successfully"}


@app.post("/api/login")
def login(form_data: OAuth2PasswordRequestForm = Depends()):
    with get_db() as conn:
        row = conn.execute(
            "SELECT id, username, display_name, password_hash FROM users WHERE username = ?",
            (form_data.username,),
        ).fetchone()

    if not row or not verify_password(form_data.password, row["password_hash"]):
        print(f"Invalid credentials\n")
        raise InvalidCredentialsException

    user = User(
        id=row["id"], username=row["username"], display_name=row["display_name"]
    )
    access_token = manager.create_access_token(
        data={"sub": user.username},
        expires=timedelta(minutes=ACCESS_TOKEN_EXPIRE_MINUTES),
    )
    resp = {"access_token": access_token, "token_type": "bearer"}
    response = Response(content=json.dumps(resp), media_type="application/json")
    manager.set_cookie(response, access_token)
    print(f"login token = {access_token}")
    return response


@app.post("/api/logout")
def logout():
    # Client simply deletes the cookie or token
    return {"message": "Logged out"}


@app.get("/api/me")
def me(user=Depends(manager)):
    return user


# ---------- Like endpoints ----------


@app.post("/api/posts/{post_id}/like")
def like_post(post_id: int, user=Depends(manager)):
    with get_db() as conn:
        post_exists = conn.execute(
            "SELECT 1 FROM posts WHERE id = ?", (post_id,)
        ).fetchone()
        if not post_exists:
            raise HTTPException(status_code=404, detail="Post not found")
        conn.execute(
            "INSERT OR IGNORE INTO likes (post_id, user_id) VALUES (?, ?)",
            (post_id, user.id),
        )
        conn.commit()

        like_count = conn.execute(
            "SELECT COUNT(*) FROM likes WHERE post_id = ?", (post_id,)
        ).fetchone()[0]
        return {"likes": like_count, "liked": True}


@app.delete("/api/posts/{post_id}/like")
def unlike_post(post_id: int, user=Depends(manager)):
    with get_db() as conn:
        conn.execute(
            "DELETE FROM likes WHERE post_id = ? AND user_id = ?",
            (post_id, user.id),
        )
        conn.commit()

        like_count = conn.execute(
            "SELECT COUNT(*) FROM likes WHERE post_id = ?", (post_id,)
        ).fetchone()[0]
        return {"likes": like_count, "liked": False}


@app.delete("/api/posts/{post_id}", status_code=204)
def delete_post(post_id: int, user=Depends(manager)):
    with get_db() as conn:
        post = conn.execute(
            "SELECT user_id FROM posts WHERE id = ?", (post_id,)
        ).fetchone()
        if not post:
            raise HTTPException(status_code=404, detail="Post not found")
        if post["user_id"] != user.id:
            raise HTTPException(
                status_code=403, detail="Not allowed to delete this post"
            )

        # Delete associated usermedia files from disk (best-effort)
        media_rows = conn.execute(
            "SELECT url FROM media WHERE post_id = ?", (post_id,)
        ).fetchall()
        for row in media_rows:
            try:
                file_path = USERMEDIA_DIR / Path(row["url"]).name
                if file_path.exists():
                    file_path.unlink()
            except Exception:
                pass  # ignore missing or permission errors

        conn.execute("DELETE FROM posts WHERE id = ?", (post_id,))
        conn.commit()


class CommentCreate(BaseModel):
    text: str


@app.post("/api/posts/{post_id}/comments", status_code=201)
def create_comment(post_id: int, payload: CommentCreate, user=Depends(manager)):
    with get_db() as conn:
        post_exists = conn.execute(
            "SELECT 1 FROM posts WHERE id = ?", (post_id,)
        ).fetchone()
        if not post_exists:
            raise HTTPException(status_code=404, detail="Post not found")

        conn.execute(
            "INSERT INTO comments (post_id, user_id, text) VALUES (?, ?, ?)",
            (post_id, user.id, payload.text),
        )
        conn.commit()
        return {"message": "Comment added"}


@app.get("/api/posts/{post_id}/comments")
def get_post_comments(
    post_id: int, page: int = 1, count: int = 15, user=Depends(manager)
):
    offset = (page - 1) * count
    with get_db() as conn:
        rows = conn.execute(
            """
            SELECT c.text, u.username
            FROM comments c
            JOIN users u ON c.user_id = u.id
            WHERE c.post_id = ?
            ORDER BY c.created_at ASC
            LIMIT ? OFFSET ?
            """,
            (post_id, count, offset),
        ).fetchall()

        comments = [Comment(author=r["username"], text=r["text"]) for r in rows]

        total = conn.execute(
            "SELECT COUNT(*) FROM comments WHERE post_id = ?", (post_id,)
        ).fetchone()[0]
        has_more = (page * count) < total

        return {"comments": comments, "has_more": has_more, "page": page}


@app.post("/api/upload")
async def upload_media(files: list[UploadFile] = File(...), user=Depends(manager)):
    """
    Accepts one or more files (images or videos) and stores them in the
    usermedia/ folder. Returns the public URLs that can be used in posts.
    """
    saved = []
    for file in files:
        if not file.content_type.startswith(("image/", "video/")):
            raise HTTPException(
                status_code=400, detail="Only image and video files are allowed"
            )

        # Create a unique filename while preserving the original extension
        ext = Path(file.filename).suffix.lower() or ".bin"
        unique_name = f"{uuid4().hex}{ext}"
        dest_path = USERMEDIA_DIR / unique_name

        # Write the file
        with open(dest_path, "wb") as f:
            content = await file.read()
            f.write(content)

        # Public URL that the browser can use
        public_url = f"/usermedia/{unique_name}"
        saved.append(
            {
                "url": public_url,
                "type": "video" if file.content_type.startswith("video/") else "image",
            }
        )

    return {"files": saved}


class MediaItem(BaseModel):
    type: str
    url: str


class Comment(BaseModel):
    author: str
    text: str


class PostResponse(BaseModel):
    id: int
    user: str
    timestamp: str
    type: str
    text: str = ""
    quote: str = ""
    media: list[MediaItem] = []
    comments: list[Comment] = []
    likes: int = 0
    liked: bool = False
    comment_count: int = 0  # total comments on the post
    owned_by_me: bool = False


class PostCreate(BaseModel):
    type: str
    text: str | None = None
    quote: str | None = None
    media: list[MediaItem] = []
    quoteStyle: str | None = None
    taggedFriends: list[str] = []


@app.post("/api/posts", response_model=PostResponse)
def create_post(payload: PostCreate, user=Depends(manager)):
    with get_db() as conn:
        # Validate allowed types
        if payload.type not in ("text", "big-text", "single-media", "multi-media"):
            raise HTTPException(status_code=400, detail="Invalid post type")

        # Insert the post
        cur = conn.execute(
            """INSERT INTO posts (user_id, type, text, quote)
               VALUES (?, ?, ?, ?)""",
            (user.id, payload.type, payload.text, payload.quote),
        )
        post_id = cur.lastrowid

        # Insert any media items
        for idx, m in enumerate(payload.media):
            conn.execute(
                """INSERT INTO media (post_id, type, url, position)
                   VALUES (?, ?, ?, ?)""",
                (post_id, m.type, m.url, idx),
            )

        conn.commit()

        # Return the freshly created post (with the same shape the feed returns)
        like_count = 0
        liked_by_me = False

        # Re-fetch the single post row we just created
        row = conn.execute(
            """
            SELECT p.id, p.type, p.text, p.quote, p.created_at, p.user_id,
                   u.username, u.display_name
            FROM posts p
            JOIN users u ON p.user_id = u.id
            WHERE p.id = ?
            """,
            (post_id,),
        ).fetchone()

        media_rows = conn.execute(
            "SELECT type, url FROM media WHERE post_id = ? ORDER BY position",
            (post_id,),
        ).fetchall()
        media = [MediaItem(type=m["type"], url=m["url"]) for m in media_rows]

        return PostResponse(
            id=post_id,
            user=row["display_name"] or row["username"],
            timestamp="Just now",
            type=row["type"],
            text=row["text"] or "",
            quote=row["quote"] or "",
            media=media,
            comments=[],
            likes=like_count,
            liked=liked_by_me,
            comment_count=0,
            owned_by_me=(row["user_id"] == user.id),
        )


@app.get("/api/posts", response_model=list[PostResponse])
def get_posts(
    page: int = Query(1, ge=1),
    count: int = Query(10, ge=1, le=50),
    user=Depends(manager),
):
    offset = (page - 1) * count

    with get_db() as conn:
        rows = conn.execute(
            """
            SELECT p.id, p.type, p.text, p.quote, p.created_at, p.user_id,
                   u.username, u.display_name
            FROM posts p
            JOIN users u ON p.user_id = u.id
            ORDER BY p.created_at DESC
            LIMIT ? OFFSET ?
            """,
            (count, offset),
        ).fetchall()

        result = []
        for row in rows:
            post_id = row["id"]

            media_rows = conn.execute(
                "SELECT type, url FROM media WHERE post_id = ? ORDER BY position",
                (post_id,),
            ).fetchall()
            media = [MediaItem(type=m["type"], url=m["url"]) for m in media_rows]

            comment_rows = conn.execute(
                """
                SELECT c.text, u.username
                FROM comments c
                JOIN users u ON c.user_id = u.id
                WHERE c.post_id = ?
                ORDER BY c.created_at ASC
                LIMIT 3
                """,
                (post_id,),
            ).fetchall()
            comments = [
                Comment(author=r["username"], text=r["text"]) for r in comment_rows
            ]

            like_count = conn.execute(
                "SELECT COUNT(*) FROM likes WHERE post_id = ?", (post_id,)
            ).fetchone()[0]

            liked_by_me = (
                conn.execute(
                    "SELECT 1 FROM likes WHERE post_id = ? AND user_id = ?",
                    (post_id, user.id),
                ).fetchone()
                is not None
            )

            total_comments = conn.execute(
                "SELECT COUNT(*) FROM comments WHERE post_id = ?", (post_id,)
            ).fetchone()[0]

            result.append(
                PostResponse(
                    id=post_id,
                    user=row["display_name"] or row["username"],
                    timestamp="Just now",
                    type=row["type"],
                    text=row["text"] or "",
                    quote=row["quote"] or "",
                    media=media,
                    comments=comments,
                    likes=like_count,
                    liked=liked_by_me,
                    comment_count=total_comments,
                    owned_by_me=(row["user_id"] == user.id),
                )
            )

        return result


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8000)

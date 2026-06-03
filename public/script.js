// script.js — Nanoserver feed with real backend + login

const API_BASE = "";
const feedContainer = document.getElementById("feed-container");
let page = 1;
let loading = false;
let hasMore = true;
let refreshing = false;

// ---------- Auth helpers ----------

async function checkAuth() {
  try {
    const res = await fetch(`${API_BASE}/api/me`, {
      credentials: "include",
    });
    if (!res.ok) {
      window.location.href = "login.html";
      return false;
    }
    return true;
  } catch (e) {
    window.location.href = "login.html";
    return false;
  }
}

// ---------- API ----------

async function fetchPosts(pageNum = 1, count = 10) {
  const res = await fetch(
    `${API_BASE}/api/posts?page=${pageNum}&count=${count}`,
    { credentials: "include" },
  );
  if (!res.ok) {
    if (res.status === 401) {
      window.location.href = "login.html";
    }
    throw new Error(`Failed to load posts: ${res.status}`);
  }
  return res.json();
}

// ---------- Rendering (unchanged from original) ----------

function renderPostContent(post) {
  const textHtml =
    post.text && post.type !== "big-text"
      ? `
        <div class="text-container">
          <div class="post-text ${post.text.length > 400 ? "truncated" : ""}">${post.text}</div>
          ${post.text.length > 400 ? '<button class="show-more">Show more</button>' : ""}
        </div>
      `
      : "";

  if (post.type === "text") {
    return textHtml;
  }
  if (post.type === "big-text") {
    return `<div class="big-text">${post.quote}</div>`;
  }

  // single-media or multi-media — render optional text + media
  let mediaHtml = "";
  if (post.type === "single-media" && post.media && post.media.length) {
    const item = post.media[0];
    if (item.type === "image") {
      mediaHtml = `<img class="post-media-single" src="${item.url}" alt="Post media">`;
    } else {
      mediaHtml = `<video class="post-media-single" controls src="${item.url}"></video>`;
    }
  } else if (post.type === "multi-media" && post.media && post.media.length) {
    const count = post.media.length;
    mediaHtml = `<div class="post-media" data-count="${count}">`;
    post.media.forEach((item) => {
      if (item.type === "image") {
        mediaHtml += `<img src="${item.url}" alt="Post media">`;
      } else {
        mediaHtml += `<video controls src="${item.url}"></video>`;
      }
    });
    mediaHtml += `</div>`;
  }

  return textHtml + mediaHtml;
}

function createPost(postData) {
  const postEl = document.createElement("div");
  postEl.className = "post";

  let commentsHtml = "";
  if (postData.comments.length > 0) {
    const more =
      postData.comment_count > postData.comments.length
        ? `<div class="more-comments-hint">${postData.comment_count} more comments...</div>`
        : "";
    commentsHtml = `
      <div class="comments">
        ${postData.comments
          .map(
            (c) =>
              `<div class="comment">
            <span class="comment-author">${c.author}</span>
            <span>${c.text}</span>
          </div>`,
          )
          .join("")}
        ${more}
      </div>`;
  }

  postEl.innerHTML = `
    <div class="post-main">
      <div class="post-header">
        <div class="avatar"></div>
        <div>
          <div class="username">${postData.user}</div>
          <div class="timestamp">${postData.timestamp}</div>
        </div>
        ${
          postData.owned_by_me
            ? `
        <div class="post-menu">
          <button class="post-menu-btn">⋯</button>
          <div class="post-menu-dropdown">
            <button class="post-menu-item delete">Delete...</button>
          </div>
        </div>`
            : ""
        }
      </div>
      <div class="post-content">
        ${renderPostContent(postData)}
      </div>
      <div class="actions">
        <button class="action-btn like-btn">${postData.liked ? "❤️" : "♡"} Like (${postData.likes || 0})</button>
        <button class="action-btn comment-btn">💬 Comment</button>
        <button class="action-btn">↗️ Share</button>
      </div>
    </div>
    ${commentsHtml}
  `;

  // Expand/collapse long text
  const textEl = postEl.querySelector(".post-text");
  const toggleBtn = postEl.querySelector(".show-more");
  if (toggleBtn && textEl) {
    toggleBtn.onclick = () => {
      if (textEl.classList.contains("truncated")) {
        textEl.classList.remove("truncated");
        textEl.classList.add("expanded");
        toggleBtn.textContent = "Show less";
        toggleBtn.className = "show-less";
      } else {
        textEl.classList.remove("expanded");
        textEl.classList.add("truncated");
        toggleBtn.textContent = "Show more";
        toggleBtn.className = "show-more";
      }
    };
  }

  // Like button handler
  const likeBtn = postEl.querySelector(".like-btn");
  if (likeBtn) {
    likeBtn.onclick = async () => {
      const postId = postData.id;
      const isLiked = likeBtn.textContent.includes("❤️");
      const method = isLiked ? "DELETE" : "POST";
      const url = `${API_BASE}/api/posts/${postId}/like`;

      try {
        const res = await fetch(url, { method, credentials: "include" });
        if (res.ok) {
          // Prefer the authoritative count + state from the backend
          let updated = null;
          try {
            updated = await res.json();
          } catch (_) {
            /* no JSON body */
          }
          if (updated && typeof updated.likes === "number") {
            const heart = updated.liked ? "❤️" : "♡";
            likeBtn.innerHTML = `${heart} Like (${updated.likes})`;
            postData.likes = updated.likes;
            postData.liked = updated.liked;
          } else {
            // Fallback optimistic update (kept for compatibility)
            const countMatch = likeBtn.textContent.match(/\((\d+)\)/);
            let count = countMatch ? parseInt(countMatch[1], 10) : 0;
            if (isLiked) {
              count = Math.max(0, count - 1);
              likeBtn.innerHTML = `♡ Like (${count})`;
            } else {
              count++;
              likeBtn.innerHTML = `❤️ Like (${count})`;
            }
          }
        }
      } catch (e) {
        console.error("Like failed", e);
      }
    };
  }

  // Comment button handler
  const commentBtn = postEl.querySelector(".comment-btn");
  if (commentBtn) {
    commentBtn.onclick = () => {
      if (window.openCommentModal) {
        window.openCommentModal(postData, postEl);
      }
    };
  }

  // Post menu (⋯) + delete
  const menuBtn = postEl.querySelector(".post-menu-btn");
  const menuDropdown = postEl.querySelector(".post-menu-dropdown");
  const deleteItem = postEl.querySelector(".post-menu-item.delete");

  if (menuBtn && menuDropdown) {
    menuBtn.onclick = (e) => {
      e.stopImmediatePropagation();
      menuDropdown.classList.toggle("show");
    };

    // Close dropdown when clicking elsewhere
    document.addEventListener("click", (e) => {
      if (!menuDropdown.contains(e.target) && !menuBtn.contains(e.target)) {
        menuDropdown.classList.remove("show");
      }
    });
  }

  if (deleteItem) {
    deleteItem.onclick = (e) => {
      e.stopImmediatePropagation();
      if (menuDropdown) menuDropdown.classList.remove("show");
      showDeleteModal(postData, postEl);
    };
  }

  return postEl;
}

// Delete confirmation modal
function showDeleteModal(postData, postEl) {
  // remove existing modal if present
  const existing = document.getElementById("delete-post-modal");
  if (existing) existing.remove();

  const modal = document.createElement("div");
  modal.id = "delete-post-modal";
  modal.innerHTML = `
    <div class="modal-overlay"></div>
    <div class="modal-content delete-modal">
      <div class="modal-header">
        <h3>Delete this post?</h3>
        <button class="modal-close">×</button>
      </div>
      <div class="delete-preview">
        <div class="post-preview">
          <div class="post-header">
            <div class="avatar"></div>
            <div>
              <div class="username">${postData.user}</div>
              <div class="timestamp">${postData.timestamp}</div>
            </div>
          </div>
          <div class="post-content-preview">
            ${renderPostContent(postData)}
          </div>
        </div>
      </div>
      <div class="modal-actions">
        <button class="btn btn-danger delete-confirm">Delete</button>
      </div>
    </div>
  `;

  document.body.appendChild(modal);

  const overlay = modal.querySelector(".modal-overlay");
  const closeBtn = modal.querySelector(".modal-close");
  const confirmBtn = modal.querySelector(".delete-confirm");

  const closeModal = () => modal.remove();

  overlay.onclick = closeModal;
  closeBtn.onclick = closeModal;

  confirmBtn.onclick = async () => {
    try {
      const res = await fetch(`${API_BASE}/api/posts/${postData.id}`, {
        method: "DELETE",
        credentials: "include",
      });
      if (res.ok) {
        postEl.remove();
        closeModal();
      } else {
        alert("Failed to delete post");
      }
    } catch (e) {
      console.error("Delete failed", e);
      alert("Failed to delete post");
    }
  };
}

// ---------- Loading & Infinite Scroll ----------

async function refreshFeedWithOverlay() {
  if (refreshing) return;
  refreshing = true;

  const originalPosition = feedContainer.style.position;
  feedContainer.style.position = "relative";

  // Create dark overlay + spinner
  const overlay = document.createElement("div");
  overlay.className = "feed-refresh-overlay";
  overlay.innerHTML = `<div class="spinner"></div>`;
  overlay.style.opacity = "0";
  feedContainer.appendChild(overlay);

  // Force reflow then fade in
  void overlay.offsetWidth;
  overlay.style.transition = "opacity 0.25s ease";
  overlay.style.opacity = "1";

  // Do the actual refresh while the overlay is up
  // (remove everything except the overlay itself)
  Array.from(feedContainer.children)
    .filter((el) => el !== overlay)
    .forEach((el) => el.remove());
  page = 1;
  hasMore = true;

  const authed = await checkAuth();
  if (!authed) {
    overlay.remove();
    feedContainer.style.position = originalPosition;
    refreshing = false;
    return;
  }

  await loadPosts();

  // Fade out + remove overlay
  overlay.style.opacity = "0";
  await new Promise((r) => setTimeout(r, 300));
  overlay.remove();

  feedContainer.style.position = originalPosition;
  refreshing = false;
}

async function loadPosts() {
  if (loading || !hasMore) return;
  loading = true;

  const loadingEl =
    document.querySelector(".loading") || document.createElement("div");
  loadingEl.className = "loading";
  loadingEl.textContent = "Loading...";
  feedContainer.appendChild(loadingEl);

  try {
    const posts = await fetchPosts(page);
    loadingEl.remove();

    if (posts.length === 0) {
      if (!document.querySelector(".end-of-feed")) {
        const endEl = document.createElement("div");
        endEl.className = "end-of-feed";
        endEl.innerHTML = `
          You're all caught up!<br>
          <button class="refresh-btn">Refresh feed</button>
        `;
        feedContainer.appendChild(endEl);

        const refreshBtn = endEl.querySelector(".refresh-btn");
        if (refreshBtn) {
          refreshBtn.onclick = refreshFeedWithOverlay;
        }
      }
      hasMore = false;
      return;
    }

    posts.forEach((postData) => {
      feedContainer.appendChild(createPost(postData));
    });

    page++;
  } catch (err) {
    loadingEl.textContent = "Error loading posts";
    console.error(err);
  } finally {
    loading = false;
  }
}

// ---------- Gallery (PhotoSwipe) ----------

function initGallery() {
  if (!window.PhotoSwipe) return;

  const Pswp = window.PhotoSwipe;

  // Use manual click handling so it works reliably with dynamically loaded posts
  // (avoids PhotoSwipeLightbox auto-binding that misses newly appended .post elements)
  feedContainer.addEventListener("click", (e) => {
    const targetImg = e.target.closest(
      "img.post-media-single, .post-media img",
    );
    if (!targetImg) return;

    // Find the containing post (each post is its own gallery group)
    const postEl = targetImg.closest(".post");
    if (!postEl) return;

    // Collect all media images belonging to this post only
    const imgs = Array.from(
      postEl.querySelectorAll("img.post-media-single, .post-media img"),
    );
    if (imgs.length === 0) return;

    // Build PhotoSwipe data source from the images inside THIS post
    const items = imgs.map((img) => ({
      src: img.getAttribute("src"),
      w: img.naturalWidth || 1200,
      h: img.naturalHeight || 1200,
    }));

    const clickedIndex = Math.max(0, imgs.indexOf(targetImg));

    // Open PhotoSwipe manually
    const pswp = new Pswp({
      dataSource: items,
      index: clickedIndex,
      bgOpacity: 0.92,
      showHideOpacity: true,
      zoom: true,
      wheelToZoom: true,
      arrowKeys: true,
      initialZoomLevel: "fit",
      secondaryZoomLevel: 2,
    });

    pswp.init();
  });
}

// ---------- Init ----------

async function init() {
  const authed = await checkAuth();
  if (!authed) return;

  const userRes = await fetch(`${API_BASE}/api/me`, { credentials: "include" });
  if (userRes.ok) {
    const user = await userRes.json();
    const nameEl = document.getElementById("username-display");
    if (nameEl) nameEl.textContent = user.display_name || user.username;

    const logoutBtn = document.getElementById("logout-btn");
    if (logoutBtn) {
      logoutBtn.onclick = async () => {
        await fetch(`${API_BASE}/api/logout`, {
          method: "POST",
          credentials: "include",
        });
        window.location.href = "login.html";
      };
    }
  }

  loadPosts();

  // Wire up Add Post button
  const addPostBtn = document.getElementById("add-post-btn");
  if (addPostBtn && window.openAddPostModal) {
    addPostBtn.onclick = window.openAddPostModal;
  }

  feedContainer.addEventListener("scroll", () => {
    const nearBottom =
      feedContainer.scrollTop + feedContainer.clientHeight >=
      feedContainer.scrollHeight - 100;

    if (nearBottom && hasMore) {
      loadPosts();
    }
  });

  // Initialize PhotoSwipe gallery (event delegation means new images work automatically)
  initGallery();
}

init();

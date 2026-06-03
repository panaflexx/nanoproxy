// comments.js — Comment modal popup for Nanoserver

let currentPostId = null;
let currentPostData = null;
let currentPostEl = null;
let commentsPage = 1;
let commentsHasMore = false;
let loadingComments = false;

function createCommentModal() {
  // Remove existing if present
  const existing = document.getElementById("comment-modal");
  if (existing) existing.remove();

  const modal = document.createElement("div");
  modal.id = "comment-modal";
  modal.innerHTML = `
    <div class="modal-overlay"></div>
    <div class="modal-content">
      <div class="modal-header">
        <h3>Comments</h3>
        <button class="modal-close">×</button>
      </div>

      <div class="modal-post-preview">
        <div class="post-header">
          <div class="avatar"></div>
          <div>
            <div class="username" id="modal-username"></div>
            <div class="timestamp" id="modal-timestamp"></div>
          </div>
        </div>
        <div class="modal-post-content" id="modal-post-content"></div>
      </div>

      <div class="modal-comments" id="modal-comments-list">
        <!-- comments populated here -->
      </div>

      <div class="modal-add-comment">
        <textarea id="new-comment-text" placeholder="Write a comment..."></textarea>
        <button id="add-comment-btn" class="add-btn">Add</button>
      </div>
    </div>
  `;

  document.body.appendChild(modal);
  document.body.style.overflow = "hidden"; // prevent feed scroll while modal is open

  // Close handlers
  modal.querySelector(".modal-close").onclick = closeCommentModal;
  modal.querySelector(".modal-overlay").onclick = closeCommentModal;

  // Add comment handler
  const addBtn = modal.querySelector("#add-comment-btn");
  const textarea = modal.querySelector("#new-comment-text");

  addBtn.onclick = async () => {
    const text = textarea.value.trim();
    if (!text || !currentPostId) return;

    try {
      const res = await fetch(
        `${API_BASE}/api/posts/${currentPostId}/comments`,
        {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          credentials: "include",
          body: JSON.stringify({ text }),
        },
      );

      if (res.ok) {
        // Append locally so the UI updates immediately
        const usernameEl = document.getElementById("username-display");
        const author = usernameEl ? usernameEl.textContent.trim() : "You";

        currentPostData.comments = currentPostData.comments || [];
        currentPostData.comments.push({ author, text });
        if (typeof currentPostData.comment_count === "number") {
          currentPostData.comment_count++;
        } else {
          currentPostData.comment_count = currentPostData.comments.length;
        }

        textarea.value = "";

        // Wait for backend propagation then refresh view
        await new Promise((r) => setTimeout(r, 500));
        await loadCommentsIntoModal();
      } else {
        alert("Failed to post comment");
      }
    } catch (e) {
      console.error("Comment post failed", e);
    }
  };

  return modal;
}

function closeCommentModal() {
  document.body.style.overflow = ""; // restore feed scroll
  const modal = document.getElementById("comment-modal");
  if (modal) modal.remove();

  if (currentPostEl && currentPostData) {
    updatePostCommentsPreview(currentPostEl, currentPostData);
  }

  currentPostId = null;
  currentPostData = null;
  currentPostEl = null;
}

async function loadCommentsIntoModal(reset = true) {
  const listEl = document.getElementById("modal-comments-list");
  if (!listEl || loadingComments) return;

  if (reset) {
    commentsPage = 1;
    commentsHasMore = false;
    listEl.innerHTML = `<div class="loading">Loading comments...</div>`;
  } else {
    // append mode – show loader at bottom
    const loader = document.createElement("div");
    loader.className = "loading more-comments";
    loader.textContent = "Loading more...";
    listEl.appendChild(loader);
  }

  loadingComments = true;

  try {
    const res = await fetch(
      `${API_BASE}/api/posts/${currentPostId}/comments?page=${commentsPage}&count=15`,
      { credentials: "include" },
    );
    if (!res.ok) throw new Error("Failed to load comments");

    const data = await res.json();
    const comments = data.comments || [];
    commentsHasMore = data.has_more || false;

    if (reset) {
      listEl.innerHTML = "";
    } else {
      // remove the 'more' loader
      listEl.querySelectorAll(".more-comments").forEach((el) => el.remove());
    }

    if (comments.length === 0 && reset) {
      const empty = document.createElement("div");
      empty.className = "no-comments";
      empty.textContent = "No comments yet. Be the first!";
      listEl.appendChild(empty);
    } else {
      comments.forEach((c) => {
        const cEl = document.createElement("div");
        cEl.className = "modal-comment";
        cEl.innerHTML = `
          <span class="comment-author">${c.author}</span>
          <span class="comment-text">${c.text}</span>
        `;
        listEl.appendChild(cEl);
      });
    }

    // Attach scroll listener once (when first loading)
    if (reset && listEl && !listEl._hasScrollListener) {
      listEl._hasScrollListener = true;
      listEl.addEventListener("scroll", () => {
        if (
          commentsHasMore &&
          !loadingComments &&
          listEl.scrollTop + listEl.clientHeight >= listEl.scrollHeight - 40
        ) {
          commentsPage++;
          loadCommentsIntoModal(false);
        }
      });
    }
  } catch (e) {
    if (reset) {
      listEl.innerHTML = `<div class="error">Failed to load comments</div>`;
    }
  } finally {
    loadingComments = false;
  }
}

async function openCommentModal(postData, postEl = null) {
  currentPostData = postData;
  currentPostId = postData.id;
  currentPostEl = postEl;

  const modal = createCommentModal();

  // Populate header info
  modal.querySelector("#modal-username").textContent = postData.user;
  modal.querySelector("#modal-timestamp").textContent = postData.timestamp;

  // Render the same post content preview
  const contentContainer = modal.querySelector("#modal-post-content");
  contentContainer.innerHTML = renderPostContent(postData);

  // Load comments
  await loadCommentsIntoModal();
}

function updatePostCommentsPreview(postEl, postData) {
  if (!postEl || !postData) return;

  // Find the comments container inside the post
  const commentsContainer = postEl.querySelector(".comments");
  const newComments = postData.comments || [];
  const total = postData.comment_count || newComments.length;

  if (newComments.length === 0) {
    if (commentsContainer) commentsContainer.remove();
    return;
  }

  const moreHint =
    total > newComments.length
      ? `<div class="more-comments-hint">${total} more comments...</div>`
      : "";

  const html =
    newComments
      .map(
        (c) =>
          `<div class="comment">
          <span class="comment-author">${c.author}</span>
          <span>${c.text}</span>
        </div>`,
      )
      .join("") + moreHint;

  if (commentsContainer) {
    commentsContainer.innerHTML = html;
  } else {
    // Re-create the comments section if it didn't exist
    const newCommentsDiv = document.createElement("div");
    newCommentsDiv.className = "comments";
    newCommentsDiv.innerHTML = html;
    // Insert after actions
    const actions = postEl.querySelector(".actions");
    if (actions && actions.parentNode) {
      actions.parentNode.insertBefore(newCommentsDiv, actions.nextSibling);
    }
  }
}

// Make openCommentModal globally available for script.js
window.openCommentModal = openCommentModal;

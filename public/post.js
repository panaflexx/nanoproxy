// post.js — Add Post popover for Nanoserver

let addPostModal = null;

function createAddPostModal() {
  // Remove existing if present
  const existing = document.getElementById("add-post-modal");
  if (existing) existing.remove();

  const modal = document.createElement("div");
  modal.id = "add-post-modal";
  modal.innerHTML = `
    <div class="modal-overlay"></div>
    <div class="modal-content add-post-content">
      <div class="modal-header">
        <h3>Create Post</h3>
        <button class="modal-close">×</button>
      </div>

      <div class="add-post-body">
        <div class="post-type-selector">
          <button class="type-btn active" data-type="text">Text</button>
          <button class="type-btn" data-type="big-text">Big Quote</button>
        </div>

        <div id="post-form-text">
          <textarea id="post-text" placeholder="What's on your mind?" rows="4"></textarea>
        </div>
        <br>
        <div class="media-section">
          <div class="media-preview" id="media-preview"></div>
          <input type="file" id="media-input" accept="image/*,video/*" multiple style="display:none;">
          <button id="add-media-btn" class="action-btn">📷 Add Photo/Video</button>
        </div>

        <div class="tag-friends">
          <label>Tag friends:</label>
          <input type="text" id="friends-input" placeholder="friend1, friend2" />
          <div class="friends-hint">Comma separated usernames</div>
        </div>
      </div>

      <div class="modal-footer">
        <button id="post-cancel-btn" class="action-btn">Cancel</button>
        <button id="post-submit-btn" class="add-btn">Post</button>
      </div>
    </div>
  `;

  document.body.appendChild(modal);

  // Close handlers
  modal.querySelector(".modal-close").onclick = closeAddPostModal;
  modal.querySelector(".modal-overlay").onclick = closeAddPostModal;
  modal.querySelector("#post-cancel-btn").onclick = closeAddPostModal;

  // Type selector - toggle Big Quote styling on the shared textarea
  const typeBtns = modal.querySelectorAll(".type-btn");
  const textArea = modal.querySelector("#post-text");
  let selectedQuoteStyle = "purple";

  // Style chooser container – always occupies the same height.
  // We populate / clear its content instead of toggling display.
  const styleChooser = document.createElement("div");
  styleChooser.className = "quote-style-chooser";
  textArea.parentNode.insertBefore(styleChooser, textArea.nextSibling);
  // start empty (Text mode) – the container still reserves its 58 px height
  showQuoteStyleButtons(false);

  function applyQuotePreview(textarea, style) {
    textarea.classList.remove(
      "quote-preview-purple",
      "quote-preview-sunset",
      "quote-preview-ocean",
      "quote-preview-forest",
      "quote-preview-elegant",
      "quote-preview-birthday",
    );
    if (style) {
      textarea.classList.add("big-quote-textarea", "quote-preview-" + style);
    }
  }

  function showQuoteStyleButtons(show) {
    if (show) {
      styleChooser.innerHTML = `
        <div class="quote-style-label">Quote style:</div>
        <div class="quote-style-buttons">
          <button type="button" class="quote-style-btn active" data-style="purple">Purple</button>
          <button type="button" class="quote-style-btn" data-style="sunset">Sunset</button>
          <button type="button" class="quote-style-btn" data-style="ocean">Ocean</button>
          <button type="button" class="quote-style-btn" data-style="forest">Forest</button>
          <button type="button" class="quote-style-btn" data-style="elegant">Elegant</button>
          <button type="button" class="quote-style-btn" data-style="birthday">Birthday</button>
        </div>
      `;
      // re-attach click handlers
      styleChooser.querySelectorAll(".quote-style-btn").forEach((btn) => {
        btn.onclick = () => {
          styleChooser
            .querySelectorAll(".quote-style-btn")
            .forEach((b) => b.classList.remove("active"));
          btn.classList.add("active");
          selectedQuoteStyle = btn.dataset.style;
          applyQuotePreview(textArea, selectedQuoteStyle);
        };
      });
    } else {
      styleChooser.innerHTML = "";
    }
  }

  typeBtns.forEach((btn) => {
    btn.onclick = () => {
      typeBtns.forEach((b) => b.classList.remove("active"));
      btn.classList.add("active");

      const type = btn.dataset.type;
      if (type === "big-text") {
        showQuoteStyleButtons(true);
        applyQuotePreview(textArea, selectedQuoteStyle);
        textArea.placeholder = "Enter your quote...";
      } else {
        showQuoteStyleButtons(false);
        textArea.classList.remove(
          "big-quote-textarea",
          "quote-preview-purple",
          "quote-preview-sunset",
          "quote-preview-ocean",
          "quote-preview-forest",
          "quote-preview-elegant",
          "quote-preview-birthday",
        );
        textArea.placeholder = "What's on your mind?";
      }
    };
  });

  // Media upload handlers
  const mediaInput = modal.querySelector("#media-input");
  const addMediaBtn = modal.querySelector("#add-media-btn");
  const previewContainer = modal.querySelector("#media-preview");
  let selectedMediaFiles = []; // maintain our own list so we can remove items

  function renderMediaPreview() {
    previewContainer.innerHTML = "";
    selectedMediaFiles.forEach((file, index) => {
      const url = URL.createObjectURL(file);
      const wrapper = document.createElement("div");
      wrapper.className = "media-thumb-wrapper";

      const el = document.createElement(
        file.type.startsWith("video") ? "video" : "img",
      );
      el.src = url;
      if (el.tagName === "VIDEO") el.controls = true;
      el.className = "media-thumb";

      const removeBtn = document.createElement("button");
      removeBtn.textContent = "×";
      removeBtn.className = "media-remove-btn";
      removeBtn.onclick = () => {
        selectedMediaFiles.splice(index, 1);
        renderMediaPreview();
      };

      wrapper.appendChild(el);
      wrapper.appendChild(removeBtn);
      previewContainer.appendChild(wrapper);
    });
  }

  addMediaBtn.onclick = () => mediaInput.click();

  mediaInput.onchange = () => {
    const newFiles = Array.from(mediaInput.files);
    // append new files (avoid duplicates by name+size for simplicity)
    newFiles.forEach((file) => {
      const exists = selectedMediaFiles.some(
        (f) => f.name === file.name && f.size === file.size,
      );
      if (!exists) selectedMediaFiles.push(file);
    });
    renderMediaPreview();
    // reset the input so the same file can be chosen again if needed
    mediaInput.value = "";
  };

  // Submit handler
  modal.querySelector("#post-submit-btn").onclick = async () => {
    const activeTypeBtn = modal.querySelector(".type-btn.active");
    const type = activeTypeBtn ? activeTypeBtn.dataset.type : "text";

    const usernameEl = document.getElementById("username-display");
    const author = usernameEl ? usernameEl.textContent.trim() : "You";

    let postData = {
      type,
      user: author,
      timestamp: "just now",
      comments: [],
      likes: 0,
      liked: false,
    };

    // Use the shared text area for both plain text and big quote
    const text = modal.querySelector("#post-text").value.trim();
    if (text) {
      if (type === "big-text") {
        postData.quote = text;
        postData.quoteStyle = selectedQuoteStyle;
      } else {
        postData.text = text;
      }
    }

    // Upload media first (if any) so we get real server URLs
    let mediaList = [];
    if (selectedMediaFiles.length > 0) {
      const formData = new FormData();
      selectedMediaFiles.forEach((f) => formData.append("files", f));

      const uploadRes = await fetch(`${API_BASE}/api/upload`, {
        method: "POST",
        credentials: "include",
        body: formData,
      });

      if (!uploadRes.ok) {
        alert("Failed to upload media");
        return;
      }

      const uploadData = await uploadRes.json();
      mediaList = uploadData.files || [];
    }

    if (mediaList.length > 0) {
      postData.media = mediaList;
      // Adjust type automatically when media is present
      if (!type.startsWith("multi") && mediaList.length > 1) {
        postData.type = "multi-media";
      } else if (type === "text" && mediaList.length === 1) {
        postData.type = "single-media";
      }
    }

    const friendsVal = modal.querySelector("#friends-input").value.trim();
    if (friendsVal) {
      postData.taggedFriends = friendsVal
        .split(",")
        .map((f) => f.trim())
        .filter(Boolean);
    }

    try {
      const res = await fetch(`${API_BASE}/api/posts`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        credentials: "include",
        body: JSON.stringify(postData),
      });

      if (res.ok) {
        // Optimistically add to top of feed
        const newPost = await res.json().catch(() => postData);
        const feedContainer = document.getElementById("feed-container");
        if (feedContainer && typeof createPost === "function") {
          const postEl = createPost(newPost);
          feedContainer.insertBefore(postEl, feedContainer.firstChild);
        }
        closeAddPostModal();
      } else {
        alert("Failed to create post");
      }
    } catch (e) {
      console.error("Post creation failed", e);
      // Fallback: still add locally for demo
      const feedContainer = document.getElementById("feed-container");
      if (feedContainer && typeof createPost === "function") {
        const postEl = createPost(postData);
        feedContainer.insertBefore(postEl, feedContainer.firstChild);
      }
      closeAddPostModal();
    }
  };

  return modal;
}

function closeAddPostModal() {
  const modal = document.getElementById("add-post-modal");
  if (modal) modal.remove();
  addPostModal = null;
}

function openAddPostModal() {
  addPostModal = createAddPostModal();
  // Default to text type
  const textSection = addPostModal.querySelector("#post-form-text");
  if (textSection) textSection.style.display = "block";
}

// Expose globally like comment modal
window.openAddPostModal = openAddPostModal;

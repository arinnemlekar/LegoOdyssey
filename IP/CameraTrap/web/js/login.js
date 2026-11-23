import { auth, USERNAME_DOMAIN } from "./common.js";
import { signInWithEmailAndPassword, onAuthStateChanged }
  from "https://www.gstatic.com/firebasejs/10.14.1/firebase-auth.js";

// Where to go after logging in (only pages on this site).
let next = new URLSearchParams(location.search).get("next") || "/";
if (!next.startsWith("/") || next.startsWith("//")) next = "/";

// Already logged in? Skip the form.
onAuthStateChanged(auth, user => { if (user) location.href = next; });

const form = document.getElementById("form");
const error = document.getElementById("error");

form.addEventListener("submit", async event => {
  event.preventDefault();
  error.hidden = true;
  document.getElementById("go").disabled = true;

  let username = document.getElementById("username").value.trim().toLowerCase();
  if (!username.includes("@")) username += "@" + USERNAME_DOMAIN;
  try {
    await signInWithEmailAndPassword(auth, username, document.getElementById("password").value);
    location.href = next;
  } catch (e) {
    error.textContent = e.code === "auth/too-many-requests"
      ? "Too many tries. Wait a few minutes and try again."
      : "Wrong username or password.";
    error.hidden = false;
    document.getElementById("go").disabled = false;
  }
});

// Ready: until now the button was disabled, so an early click couldn't reload the page
// and wipe what was typed.
document.getElementById("go").disabled = false;
document.getElementById("go").textContent = "Log in";

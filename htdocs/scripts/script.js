document.addEventListener("DOMContentLoaded", () => {
  console.log("ExploreWorld homepage loaded");

  const buttons = document.querySelectorAll(".buttons a");
  buttons.forEach(btn => {
    btn.addEventListener("click", () => {
      console.log(`Clicked: ${btn.getAttribute("href")}`);
    });
  });
});

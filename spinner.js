const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const clearScreen = () => process.stdout.write("\x1b[H\x1b[2J\x1b[3J");
import readline from "readline";

clearScreen();

function spinner() {
  const str = "-\b|\b/\b-\b\\\b|\b/\b";
  let i = 0;
  let isRunning = true;

  const stop = () => {
    clearScreen();
    isRunning = false;
  };

  (async () => {
    while (isRunning) {
      if (i === str.length) {
        i = 0;
      }
      process.stdout.write(str[i]);
      i += 1;
      await delay(350);
    }
  })();

  return stop;
}

async function interactiveExample() {
  const rl = readline.createInterface({
    input: process.stdin,
    output: process.stdout,
  });

  console.log("Starting spinner... Press Enter to stop it");
  const stopSpinner = spinner();

  // Wait for user to press Enter
  await new Promise((resolve) => {
    rl.once("line", () => {
      stopSpinner();
      console.log("\nSpinner stopped!");
      rl.close();
      resolve();
    });
  });
}

async function simulateLoading() {
  console.log("Loading data...");
  const stopSpinner = spinner();

  try {
    // Simulate some async work
    await delay(5000);
    console.log("\nFirst step complete");

    await delay(2500);
    console.log("\nSecond step complete");

    await delay(1000);
    console.log("\nDone loading!");
  } finally {
    stopSpinner();
  }
}

// Run the examples sequentially
await interactiveExample();
await simulateLoading();

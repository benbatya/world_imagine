#!/usr/bin/env node
// pixelmatch_compare.js <screenshot.png> <reference.jpg> <threshold>
//
// Compares two images using pixelmatch. Resizes one to match the other if needed.
// Threshold = minimum fraction of matching pixels required (0.0–1.0).
// Prints "PASS <similarity>" or "FAIL <similarity>" to stdout and exits 0/1.

const fs    = require('fs');
const path  = require('path');
const { PNG } = require('pngjs');
const pixelmatch = require('pixelmatch');
const { execSync } = require('child_process');

const [,, screenshotPath, referencePath, thresholdStr] = process.argv;

if (!screenshotPath || !referencePath || !thresholdStr) {
    console.error('Usage: pixelmatch_compare.js <screenshot.png> <reference.jpg> <threshold>');
    process.exit(2);
}

const threshold = parseFloat(thresholdStr);

// Convert reference image (possibly JPEG) to PNG using ffmpeg, then compare.
const tmpRef = '/tmp/pixelmatch_ref.png';

try {
    execSync(`ffmpeg -y -i ${JSON.stringify(referencePath)} ${JSON.stringify(tmpRef)} 2>/dev/null`);
} catch (e) {
    console.error('Failed to convert reference image to PNG:', e.message);
    process.exit(2);
}

function readPNG(filePath) {
    const data = fs.readFileSync(filePath);
    return PNG.sync.read(data);
}

let img1, img2;
try {
    img1 = readPNG(screenshotPath);
} catch (e) {
    console.error('Failed to read screenshot:', e.message);
    process.exit(2);
}

try {
    img2 = readPNG(tmpRef);
} catch (e) {
    console.error('Failed to read reference image:', e.message);
    process.exit(2);
}

// Resize img2 to match img1 dimensions if they differ.
// We do this by re-encoding img2 with ffmpeg at img1's resolution.
if (img1.width !== img2.width || img1.height !== img2.height) {
    const tmpResized = '/tmp/pixelmatch_ref_resized.png';
    try {
        execSync(
            `ffmpeg -y -i ${JSON.stringify(tmpRef)} -vf scale=${img1.width}:${img1.height} ${JSON.stringify(tmpResized)} 2>/dev/null`
        );
        img2 = readPNG(tmpResized);
    } catch (e) {
        console.error('Failed to resize reference image:', e.message);
        process.exit(2);
    }
}

const { width, height } = img1;
const totalPixels = width * height;

// pixelmatch options: threshold 0.1 = allow 10% colour difference per pixel
const diff = new PNG({ width, height });
const numDiff = pixelmatch(img1.data, img2.data, diff.data, width, height, {
    threshold: 0.15,   // per-pixel colour tolerance (15%)
    includeAA: false,  // ignore anti-aliased pixels
});

const matchFraction = 1.0 - numDiff / totalPixels;
const matchPct = (matchFraction * 100).toFixed(2);

if (matchFraction >= threshold) {
    console.log(`PASS ${matchPct}% pixels match (required >= ${(threshold * 100).toFixed(0)}%)`);
    process.exit(0);
} else {
    console.log(`FAIL ${matchPct}% pixels match (required >= ${(threshold * 100).toFixed(0)}%)`);
    process.exit(1);
}

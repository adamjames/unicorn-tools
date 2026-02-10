// 4D HYPERCUBE - JavaScript version (fullscreen)

const ROTATION_SPEED_Y = 0.5;
const ROTATION_SPEED_X = 0.5;
const ROTATION_SPEED_W = 1;
const UNEVEN_TIME = true;
const DRAW_VERTICES = true;
const DRAW_EDGES = true;
const CUBE_SIZE = 0.5;
const BG_R = 8, BG_G = 8, BG_B = 16;

const S = CUBE_SIZE;

const vertices = [
  {x: S, y: S, z: S, w:-S}, {x:-S, y: S, z: S, w:-S},
  {x:-S, y:-S, z: S, w:-S}, {x: S, y:-S, z: S, w:-S},
  {x: S, y: S, z:-S, w:-S}, {x:-S, y: S, z:-S, w:-S},
  {x:-S, y:-S, z:-S, w:-S}, {x: S, y:-S, z:-S, w:-S},
  {x: S, y: S, z: S, w: S}, {x:-S, y: S, z: S, w: S},
  {x:-S, y:-S, z: S, w: S}, {x: S, y:-S, z: S, w: S},
  {x: S, y: S, z:-S, w: S}, {x:-S, y: S, z:-S, w: S},
  {x:-S, y:-S, z:-S, w: S}, {x: S, y:-S, z:-S, w: S},
];

const edges = [
  [0,1], [1,2], [2,3], [3,0], [4,5], [5,6], [6,7], [7,4],
  [0,4], [1,5], [2,6], [3,7],
  [8,9], [9,10], [10,11], [11,8], [12,13], [13,14], [14,15], [15,12],
  [8,12], [9,13], [10,14], [11,15],
  [0,8], [1,9], [2,10], [3,11], [4,12], [5,13], [6,14], [7,15]
];

// Dynamic dimensions
let WIDTH, HEIGHT, CENTER_X, CENTER_Y, SCALE, CAMERA_DIST;
let fb_r, fb_g, fb_b;
let projected = [];

function resize() {
  WIDTH = window.innerWidth;
  HEIGHT = window.innerHeight;
  CENTER_X = WIDTH / 2;
  CENTER_Y = HEIGHT / 2;
  SCALE = Math.min(WIDTH, HEIGHT) * 0.4;
  CAMERA_DIST = 1.8;
  fb_r = new Array(WIDTH * HEIGHT).fill(0);
  fb_g = new Array(WIDTH * HEIGHT).fill(0);
  fb_b = new Array(WIDTH * HEIGHT).fill(0);
  canvas.width = WIDTH;
  canvas.height = HEIGHT;
}

function rotate_xz(p, a) {
  const c = Math.cos(a), s = Math.sin(a);
  return { x: p.x*c - p.z*s, y: p.y, z: p.x*s + p.z*c, w: p.w };
}

function rotate_yz(p, a) {
  const c = Math.cos(a), s = Math.sin(a);
  return { x: p.x, y: p.y*c - p.z*s, z: p.y*s + p.z*c, w: p.w };
}

function rotate_zw(p, a) {
  const c = Math.cos(a), s = Math.sin(a);
  return { x: p.x, y: p.y, z: p.z*c - p.w*s, w: p.z*s + p.w*c };
}

const rotate_x = rotate_yz;
const rotate_y = rotate_xz;
const rotate_w = rotate_zw;

function project(p) {
  let z = p.z + CAMERA_DIST;
  if (z < 0.1) z = 0.1;
  return { x: CENTER_X + p.x/z*SCALE, y: CENTER_Y - p.y/z*SCALE };
}

function draw_line_canvas(x0, y0, x1, y1, r, g, b) {
  ctx.strokeStyle = "rgb(" + r + "," + g + "," + b + ")";
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(x0, y0);
  ctx.lineTo(x1, y1);
  ctx.stroke();
}

function draw_vertex_canvas(x, y, r, g, b) {
  ctx.fillStyle = "rgb(" + r + "," + g + "," + b + ")";
  ctx.beginPath();
  ctx.arc(x, y, 4, 0, Math.PI * 2);
  ctx.fill();
}

function render(t) {
  ctx.fillStyle = "rgb(" + BG_R + "," + BG_G + "," + BG_B + ")";
  ctx.fillRect(0, 0, WIDTH, HEIGHT);

  const ay = t * ROTATION_SPEED_Y;
  const ax = t * ROTATION_SPEED_X;
  let aw = t * ROTATION_SPEED_W;
  if (UNEVEN_TIME) aw = t * ROTATION_SPEED_W + Math.sin(t) * CUBE_SIZE;

  projected = [];
  for (let i = 0; i < vertices.length; i++) {
    let r = rotate_x(rotate_w(rotate_y(vertices[i], ay), aw), ax);
    projected[i] = project(r);
  }

  if (DRAW_EDGES) {
    for (let i = 0; i < edges.length; i++) {
      const e = edges[i];
      let r, g, b;
      if (e[0] <= 7 && e[1] <= 7) { r = 60; g = 255; b = 60; }
      else if (e[0] >= 8 && e[1] >= 8) { r = 255; g = 60; b = 60; }
      else { r = 60; g = 60; b = 255; }
      draw_line_canvas(projected[e[0]].x, projected[e[0]].y, projected[e[1]].x, projected[e[1]].y, r, g, b);
    }
  }

  if (DRAW_VERTICES) {
    for (let i = 0; i < projected.length; i++) {
      const p = projected[i];
      let r, g, b;
      if (i <= 7) { r = 100; g = 255; b = 100; }
      else { r = 255; g = 100; b = 100; }
      draw_vertex_canvas(p.x, p.y, r, g, b);
    }
  }
}

const canvas = document.createElement("canvas");
document.body.appendChild(canvas);
const ctx = canvas.getContext("2d");

resize();
window.addEventListener("resize", resize);

let startTime = performance.now();

function animate() {
  const t = (performance.now() - startTime) / 1000;
  render(t);
  requestAnimationFrame(animate);
}

animate();

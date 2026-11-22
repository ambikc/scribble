// vision.worker.ts - runs vision.ts off the main thread, like the firmware's vision task on core 0.
// The pipeline is pure, so the same code runs here and in tests; the worker only moves data.
import { visionProcess, visionRethreshold, VisionParams, VisionWork } from './vision';

type Msg = { id: number; kind: 'process'; vga: Uint16Array; params: VisionParams } | { id: number; kind: 'rethreshold'; work: VisionWork; params: VisionParams };

self.onmessage = (e: MessageEvent<Msg>) => {
  const m = e.data;
  const r = m.kind === 'process' ? visionProcess(m.vga, m.params) : visionRethreshold(m.work, m.params);
  (self as unknown as Worker).postMessage({ id: m.id, result: r });
};

// RTCRtpScriptTransform worker: runs the decode-order checker on each
// received encoded video frame and passes the frame through unchanged.
import { H264OrderChecker } from './h264_order.js';

const checker = new H264OrderChecker();

onrtctransform = (event) => {
  const t = event.transformer;
  t.readable
    .pipeThrough(new TransformStream({
      transform(frame, controller) {
        checker.onFrame(new Uint8Array(frame.data), frame.timestamp);
        controller.enqueue(frame);
      },
    }))
    .pipeTo(t.writable);
};

setInterval(() => postMessage(checker.stats), 500);

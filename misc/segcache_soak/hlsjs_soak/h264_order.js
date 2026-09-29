/**
 * H.264 decode-order checker for encoded frames as received by a WebRTC
 * receiver (before decoding).
 *
 * Invariant (H.264 7.4.3, gaps_in_frame_num_value_allowed_flag == 0): every
 * non-IDR picture has frame_num == (PrevRefFrameNum + 1) % MaxFrameNum, where
 * PrevRefFrameNum is the frame_num of the previous reference picture in
 * decode order. A B-frame sent before the P-frame it follows in decode order
 * shows up as a frame_num jump, so any reordering or missing reference frame
 * is a violation.
 */

class BitReader {
  constructor(bytes) {
    this.bytes = bytes;
    this.pos = 0; // bit position
  }

  u(n) {
    let v = 0;
    for (let i = 0; i < n; i++) {
      const byte = this.bytes[this.pos >> 3];
      if (byte === undefined) throw new Error('bitstream overrun');
      v = (v << 1) | ((byte >> (7 - (this.pos & 7))) & 1);
      this.pos++;
    }
    return v >>> 0;
  }

  ue() {
    let zeros = 0;
    while (this.u(1) === 0) {
      if (++zeros > 31) throw new Error('bad ue(v)');
    }
    return zeros === 0 ? 0 : (2 ** zeros - 1) + this.u(zeros);
  }

  se() {
    const k = this.ue();
    return k & 1 ? (k + 1) / 2 : -(k / 2);
  }
}

// Strip emulation prevention bytes (00 00 03 -> 00 00), up to `limit` output bytes.
function toRbsp(nal, limit = Infinity) {
  const out = [];
  let zeros = 0;
  for (let i = 1; i < nal.length && out.length < limit; i++) {
    const b = nal[i];
    if (zeros >= 2 && b === 3) {
      zeros = 0;
      continue;
    }
    zeros = b === 0 ? zeros + 1 : 0;
    out.push(b);
  }
  return new Uint8Array(out);
}

// Split an Annex B access unit into NAL units (start codes removed).
function splitAnnexB(data) {
  const starts = [];
  for (let i = 0; i + 2 < data.length; i++) {
    if (data[i] === 0 && data[i + 1] === 0 && data[i + 2] === 1) {
      starts.push(i + 3);
      i += 2;
    }
  }
  const nals = [];
  for (let s = 0; s < starts.length; s++) {
    let end = s + 1 < starts.length ? starts[s + 1] - 3 : data.length;
    while (end > starts[s] && data[end - 1] === 0) end--; // trailing zero of 4-byte start code
    if (end > starts[s]) nals.push(data.subarray(starts[s], end));
  }
  return nals;
}

const HIGH_PROFILES = new Set([100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135]);

function parseSps(nal) {
  const r = new BitReader(toRbsp(nal));
  const profile = r.u(8);
  r.u(8); // constraint flags
  r.u(8); // level
  const id = r.ue();
  let separateColourPlane = 0;
  if (HIGH_PROFILES.has(profile)) {
    const chroma = r.ue();
    if (chroma === 3) separateColourPlane = r.u(1);
    r.ue(); // bit_depth_luma_minus8
    r.ue(); // bit_depth_chroma_minus8
    r.u(1); // qpprime_y_zero_transform_bypass_flag
    if (r.u(1)) {
      // seq_scaling_matrix_present_flag
      const lists = chroma === 3 ? 12 : 8;
      for (let i = 0; i < lists; i++) {
        if (!r.u(1)) continue;
        const size = i < 6 ? 16 : 64;
        let last = 8;
        let next = 8;
        for (let j = 0; j < size; j++) {
          if (next !== 0) next = (last + r.se() + 256) % 256;
          last = next === 0 ? last : next;
        }
      }
    }
  }
  const log2MaxFrameNum = r.ue() + 4;
  const pocType = r.ue();
  if (pocType === 0) {
    r.ue();
  } else if (pocType === 1) {
    r.u(1);
    r.se();
    r.se();
    const n = r.ue();
    for (let i = 0; i < n; i++) r.se();
  }
  r.ue(); // max_num_ref_frames
  const gapsAllowed = r.u(1);
  return { id, log2MaxFrameNum, separateColourPlane, gapsAllowed };
}

function parsePps(nal) {
  const r = new BitReader(toRbsp(nal, 16));
  return { id: r.ue(), spsId: r.ue() };
}

export class H264OrderChecker {
  constructor() {
    this.sps = new Map();
    this.pps = new Map();
    this.prevRefFrameNum = null;
    this.stats = {
      frames: 0, // encoded frames seen
      checked: 0, // non-IDR frames checked against the invariant
      idr: 0,
      violations: 0,
      parseErrors: 0,
      gapsAllowed: false,
      examples: [], // first few violations
      recent: [], // last few frames before the first violation, for context
    };
  }

  onFrame(data, rtpTimestamp) {
    this.stats.frames++;
    let slice = null;
    try {
      for (const nal of splitAnnexB(data)) {
        const type = nal[0] & 0x1f;
        if (type === 7) {
          const s = parseSps(nal);
          this.sps.set(s.id, s);
          if (s.gapsAllowed) this.stats.gapsAllowed = true;
        } else if (type === 8) {
          const p = parsePps(nal);
          this.pps.set(p.id, p);
        } else if ((type === 1 || type === 5) && slice === null) {
          slice = nal;
        }
      }
      if (slice === null) return;

      const type = slice[0] & 0x1f;
      const refIdc = (slice[0] >> 5) & 3;
      const r = new BitReader(toRbsp(slice, 32));
      r.ue(); // first_mb_in_slice
      const sliceType = r.ue() % 5;
      const pps = this.pps.get(r.ue());
      const sps = pps && this.sps.get(pps.spsId);
      if (!sps) return; // no parameter sets yet
      if (sps.separateColourPlane) r.u(2);
      const frameNum = r.u(sps.log2MaxFrameNum);
      const entry = { n: this.stats.frames, ts: rtpTimestamp, type: 'PBI'[sliceType] ?? '?', ref: refIdc, frameNum };

      if (type === 5) {
        this.stats.idr++;
        this.prevRefFrameNum = frameNum;
      } else if (this.prevRefFrameNum !== null) {
        this.stats.checked++;
        const expected = (this.prevRefFrameNum + 1) % (1 << sps.log2MaxFrameNum);
        if (frameNum !== expected && !sps.gapsAllowed) {
          this.stats.violations++;
          entry.expected = expected;
          if (this.stats.examples.length < 10) this.stats.examples.push(entry);
        }
        if (refIdc !== 0) this.prevRefFrameNum = frameNum;
      }

      if (this.stats.violations === 0) {
        this.stats.recent.push(entry);
        if (this.stats.recent.length > 8) this.stats.recent.shift();
      }
    } catch (e) {
      this.stats.parseErrors++;
    }
  }
}

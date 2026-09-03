/**
 * JH 游戏存档密钥 Frida 动态抓取脚本
 *
 * 目标：libcocos2dcpp.so
 * 存档 dat.json 加解密链：
 *   JhUtility::getPaPaNew2 / getPaPa  -> 16 字节 XXTEA 密钥
 *   JHCrypto::decryptXXTEA / encryptXXTEA + Base64
 *   JhUtility::D_TEST / E_BTN
 *
 * 用法（USB 连接 Android 设备 / 模拟器）：
 *   frida -U -f com.xxx.game -l scripts/frida_save_crypto.js --no-pause
 *   或附加：frida -U com.xxx.game -l scripts/frida_save_crypto.js
 *
 * 触发：登录 -> 读档/下载云存档/切换角色，观察控制台输出 KEY 与 dat.json 明文片段。
 *
 * RPC（另开 frida 交互）：
 *   rpc.exports.getLastKey()
 *   rpc.exports.getHistory()
 *   rpc.exports.getSaveIndex()
 */

'use strict';

const LIB_NAME = 'libcocos2dcpp.so';
const SYM = {
  getPaPaNew2: '_ZN9JhUtility11getPaPaNew2EPci',
  getPaPa: '_ZN9JhUtility7getPaPaEPci',
  D_TEST: '_ZN9JhUtility6D_TESTEiPKciRSsbb',
  E_BTN: '_ZN9JhUtility5E_BTNEiPKciRSs',
  decryptXXTEA: '_ZN8JHCrypto12decryptXXTEAEPhiS0_iPi',
  encryptXXTEA: '_ZN8JHCrypto12encryptXXTEAEPhiS0_iPi',
  getSaveIndex: '_ZN6JhData12getSaveIndexEv',
  addMail: '_ZN6JhData7addMailEiPKcRSt3mapIiiSt4lessIiESaISt4pairIKiiEEE',
  recvMail: '_ZN6JhData8recvMailEPKc',
};

const state = {
  lastKey: null,
  lastKeySource: null,
  lastSaveIndex: null,
  history: [],
  datPlainPreview: null,
};

function log(msg) {
  console.log('[JH-SAVE] ' + msg);
}

function hexdump16(ptr, len) {
  try {
    return hexdump(ptr, { length: len, ansi: false });
  } catch (e) {
    return String(ptr);
  }
}

// libc++ std::string (Android ARM64 常见布局)
function readCppString(strPtr) {
  if (strPtr.isNull()) return null;
  try {
    const b0 = strPtr.readU8();
    if ((b0 & 1) === 0) {
      const size = b0 >> 1;
      if (size === 0) return '';
      return strPtr.add(1).readUtf8String(size);
    }
    const size = strPtr.add(8).readUInt();
    const data = strPtr.readPointer();
    if (data.isNull() || size === 0) return '';
    if (size > 8 * 1024 * 1024) return '<too-large:' + size + '>';
    return data.readUtf8String(size);
  } catch (e) {
    return null;
  }
}

function recordKey(key, source, saveIndex) {
  if (!key || key.length === 0) return;
  const entry = {
    ts: Date.now(),
    key: key,
    source: source,
    saveIndex: saveIndex,
  };
  state.lastKey = key;
  state.lastKeySource = source;
  if (saveIndex !== null && saveIndex !== undefined) {
    state.lastSaveIndex = saveIndex;
  }
  state.history.push(entry);
  if (state.history.length > 50) state.history.shift();
  log('KEY capture source=' + source + ' saveIndex=' + saveIndex + ' key="' + key + '" len=' + key.length);
  if (saveIndex !== null && saveIndex !== undefined && key.length >= 16) {
    log('Paste to server: POST /admin/api/save-key {"save_index":' + saveIndex + ',"key":"' + key.substring(0, 16) + '"}');
  }
}

function readKeyBuffer(outPtr) {
  try {
    const bytes = outPtr.readByteArray(16);
    if (!bytes) return null;
    let s = '';
    for (let i = 0; i < bytes.byteLength; i++) {
      const c = bytes[i];
      if (c === 0) break;
      s += String.fromCharCode(c);
    }
    return s.length > 0 ? s : null;
  } catch (e) {
    return null;
  }
}

function hookGetPaPaNew2(base) {
  const addr = base.findExportByName(SYM.getPaPaNew2);
  if (!addr) return log('WARN: missing ' + SYM.getPaPaNew2);
  Interceptor.attach(addr, {
    onEnter(args) {
      this.outPtr = args[0];
      this.saveIndex = args[1].toInt32();
    },
    onLeave() {
      const key = readKeyBuffer(this.outPtr);
      recordKey(key, 'getPaPaNew2', this.saveIndex);
    },
  });
  log('hooked getPaPaNew2 @ ' + addr);
}

function hookGetPaPa(base) {
  const addr = base.findExportByName(SYM.getPaPa);
  if (!addr) return log('WARN: missing ' + SYM.getPaPa);
  Interceptor.attach(addr, {
    onEnter(args) {
      this.outPtr = args[0];
      this.saveIndex = args[1].toInt32();
    },
    onLeave() {
      const key = readKeyBuffer(this.outPtr);
      recordKey(key, 'getPaPa', this.saveIndex);
    },
  });
  log('hooked getPaPa @ ' + addr);
}

function hookDTest(base) {
  const addr = base.findExportByName(SYM.D_TEST);
  if (!addr) return log('WARN: missing ' + SYM.D_TEST);
  Interceptor.attach(addr, {
    onEnter(args) {
      this.saveIndex = args[0].toInt32();
      this.cipherPtr = args[1];
      this.cipherLen = args[2].toInt32();
      this.outStr = args[3];
      this.usePaPa = args[4].toInt32();
      this.usePaPaNew2 = args[5].toInt32();
      log('D_TEST enter saveIndex=' + this.saveIndex +
          ' len=' + this.cipherLen +
          ' usePaPa=' + this.usePaPa +
          ' usePaPaNew2=' + this.usePaPaNew2);
    },
    onLeave() {
      const plain = readCppString(this.outStr);
      if (!plain) return;
      if (plain === 'error') {
        log('D_TEST decrypt error saveIndex=' + this.saveIndex);
        return;
      }
      const preview = plain.substring(0, Math.min(plain.length, 240));
      state.datPlainPreview = preview;
      log('D_TEST plain len=' + plain.length + ' preview=' + preview);
      if (plain.indexOf('myGift') >= 0) {
        log('D_TEST contains myGift — mailbox data present');
      }
    },
  });
  log('hooked D_TEST @ ' + addr);
}

function hookEBtn(base) {
  const addr = base.findExportByName(SYM.E_BTN);
  if (!addr) return log('WARN: missing ' + SYM.E_BTN);
  Interceptor.attach(addr, {
    onEnter(args) {
      this.saveIndex = args[0].toInt32();
      this.plainPtr = args[2];
      this.plainLen = args[3].toInt32();
      this.outStr = args[4];
      log('E_BTN encrypt saveIndex=' + this.saveIndex + ' plainLen=' + this.plainLen);
    },
    onLeave() {
      const cipher = readCppString(this.outStr);
      if (cipher) {
        log('E_BTN cipher b64 len=' + cipher.length + ' head=' + cipher.substring(0, 48));
      }
    },
  });
  log('hooked E_BTN @ ' + addr);
}

function hookDecryptXXTEA(base) {
  const addr = base.findExportByName(SYM.decryptXXTEA);
  if (!addr) return log('WARN: missing ' + SYM.decryptXXTEA);
  Interceptor.attach(addr, {
    onEnter(args) {
      this.outPtr = args[0];
      this.inLen = args[1].toInt32();
      this.keyPtr = args[2];
      this.keyLen = args[3].toInt32();
      let key = '';
      try {
        key = this.keyPtr.readUtf8String(this.keyLen);
      } catch (e) {
        key = hexdump16(this.keyPtr, Math.min(this.keyLen, 16));
      }
      recordKey(key, 'decryptXXTEA', state.lastSaveIndex);
      log('decryptXXTEA inLen=' + this.inLen + ' keyLen=' + this.keyLen + ' key="' + key + '"');
    },
    onLeave(retval) {
      if (retval.isNull()) {
        log('decryptXXTEA failed');
        return;
      }
      try {
        const s = retval.readUtf8String(128);
        if (s) log('decryptXXTEA out head=' + s.substring(0, 120));
      } catch (e) {}
    },
  });
  log('hooked decryptXXTEA @ ' + addr);
}

function hookEncryptXXTEA(base) {
  const addr = base.findExportByName(SYM.encryptXXTEA);
  if (!addr) return log('WARN: missing ' + SYM.encryptXXTEA);
  Interceptor.attach(addr, {
    onEnter(args) {
      this.keyPtr = args[2];
      this.keyLen = args[3].toInt32();
      let key = '';
      try {
        key = this.keyPtr.readUtf8String(this.keyLen);
      } catch (e) {}
      recordKey(key, 'encryptXXTEA', state.lastSaveIndex);
    },
  });
  log('hooked encryptXXTEA @ ' + addr);
}

function hookGetSaveIndex(base) {
  const addr = base.findExportByName(SYM.getSaveIndex);
  if (!addr) return log('WARN: missing ' + SYM.getSaveIndex);
  Interceptor.attach(addr, {
    onLeave(retval) {
      const idx = retval.toInt32();
      state.lastSaveIndex = idx;
      log('getSaveIndex -> ' + idx);
    },
  });
  log('hooked getSaveIndex @ ' + addr);
}

function hookAddMail(base) {
  const addr = base.findExportByName(SYM.addMail);
  if (!addr) return log('WARN: missing ' + SYM.addMail);
  Interceptor.attach(addr, {
    onEnter(args) {
      this.type = args[1].toInt32();
      this.desp = args[2].readUtf8String();
      log('addMail type=' + this.type + ' desp=' + this.desp);
    },
  });
  log('hooked addMail @ ' + addr);
}

function hookRecvMail(base) {
  const addr = base.findExportByName(SYM.recvMail);
  if (!addr) return log('WARN: missing ' + SYM.recvMail);
  Interceptor.attach(addr, {
    onEnter(args) {
      const mailId = args[1].readUtf8String();
      log('recvMail claim mailId=' + mailId);
    },
  });
  log('hooked recvMail @ ' + addr);
}

function scanLegacySeed(base) {
  const seed = 'ab1234abab1234ab';
  Memory.scan(base.base, base.size, seed, {
    onMatch(address, size) {
      log('found legacy seed @ ' + address);
    },
    onComplete() {},
  });
}

function installHooks() {
  const base = Process.findModuleByName(LIB_NAME);
  if (!base) {
    log('waiting for ' + LIB_NAME + ' ...');
    return false;
  }
  log('module ' + LIB_NAME + ' base=' + base.base + ' size=' + base.size);
  hookGetPaPaNew2(base);
  hookGetPaPa(base);
  hookDTest(base);
  hookEBtn(base);
  hookDecryptXXTEA(base);
  hookEncryptXXTEA(base);
  hookGetSaveIndex(base);
  hookAddMail(base);
  hookRecvMail(base);
  scanLegacySeed(base);
  return true;
}

function waitForLib() {
  if (installHooks()) return;
  const timer = setInterval(function () {
    if (installHooks()) clearInterval(timer);
  }, 500);
}

rpc.exports = {
  getLastKey: function () {
    return {
      key: state.lastKey,
      source: state.lastKeySource,
      saveIndex: state.lastSaveIndex,
    };
  },
  getHistory: function () {
    return state.history;
  },
  getSaveIndex: function () {
    return state.lastSaveIndex;
  },
  getDatPreview: function () {
    return state.datPlainPreview;
  },
};

setImmediate(waitForLib);

/************************************************************
 * DurableQueue — append-only line outbox on LittleFS
 *
 * Store-and-forward for an uplink that is sometimes down (MQTT replay,
 * Influx overflow). One file holds lines in arrival order; a second file
 * holds the byte offset of the first line not yet delivered.
 *
 *   append()     any task, one short file append under a mutex
 *   readBatch()  the uplink's own task, copies up to N lines from the cursor
 *   commit()     after the uplink accepted them, persists the new cursor;
 *                when the cursor reaches the end both files are removed
 *
 * Nothing is rewritten, so a writer never waits behind a long copy, and
 * the cursor survives a reset. Delivery is at-least-once: lines read but
 * not yet committed when power drops are sent again after boot, so every
 * line must carry an identity the receiver can de-duplicate on
 * (boot_id + seq for MQTT; measurement + tags + timestamp for Influx).
 ************************************************************/

#ifndef SMART_GARDEN_DURABLE_QUEUE_H
#define SMART_GARDEN_DURABLE_QUEUE_H

#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "flash_led_guard.h"

class DurableQueue {
 public:
  struct Batch {
    int lines = 0;
    size_t endOffset = 0;  // pass to commit() once the lines are delivered
  };

  DurableQueue(const char* path, const char* cursorPath, size_t maxBytes)
      : path_(path), cursorPath_(cursorPath), maxBytes_(maxBytes) {}

  const char* path() const { return path_; }
  size_t maxBytes() const { return maxBytes_; }
  unsigned long appended() const { return appended_; }
  unsigned long rejected() const { return rejected_; }
  unsigned long delivered() const { return delivered_; }

  // false when the file is at its cap or the write failed. The caller keeps
  // the line elsewhere (the on-device CSV journal always has every reading).
  bool append(const char* line) {
    if (line == nullptr || line[0] == '\0') {
      return false;
    }
    lock();
    bool ok = false;
    File f = LittleFS.open(path_, FILE_APPEND);
    if (f) {
      if (f.size() < maxBytes_) {
        const size_t n = strlen(line);
        ok = f.write(reinterpret_cast<const uint8_t*>(line), n) == n &&
             f.write('\n') == 1;
      }
      f.close();
    }
    if (ok) {
      appended_++;
    } else {
      rejected_++;
    }
    unlock();
    return ok;
  }

  // Bytes waiting after the cursor. 0 when empty.
  size_t pendingBytes() {
    lock();
    const size_t size = fileSize();
    const size_t cur = cursor();
    unlock();
    return size > cur ? size - cur : 0;
  }

  // Copy up to maxLines complete lines from the cursor into buf, each
  // NUL-terminated and packed back to back. Returns the batch; lines == 0
  // when there is nothing to send. A line longer than lineMax is skipped
  // (committed past) so one corrupt line cannot block the queue.
  Batch readBatch(char* buf, size_t bufLen, int maxLines, size_t lineMax) {
    Batch b;  // endOffset stays 0 when the cursor did not move
    lock();
    File f = LittleFS.open(path_, FILE_READ);
    if (!f) {
      unlock();
      return b;
    }
    size_t offset = cursor();
    if (offset > f.size()) {
      offset = 0;  // file was replaced under a stale cursor
    }
    f.seek(offset);
    size_t used = 0;
    while (b.lines < maxLines && f.available()) {
      if (bufLen - used < lineMax + 1) {
        break;
      }
      const size_t start = used;
      size_t len = 0;
      bool overflow = false;
      int c;
      while ((c = f.read()) >= 0 && c != '\n') {
        if (len < lineMax) {
          buf[start + len] = static_cast<char>(c);
          len++;
        } else {
          overflow = true;
        }
      }
      if (c < 0) {
        break;  // partial last line (append in progress): leave it
      }
      offset = f.position();
      if (overflow || len == 0) {
        b.endOffset = offset;
        continue;
      }
      if (buf[start + len - 1] == '\r') {
        len--;
      }
      buf[start + len] = '\0';
      used += len + 1;
      b.lines++;
      b.endOffset = offset;
    }
    f.close();
    unlock();
    return b;
  }

  // Persist the cursor after a batch was delivered. Drops both files once
  // everything has been sent.
  void commit(const Batch& b) {
    if (b.endOffset == 0) {
      return;
    }
    lock();
    delivered_ += static_cast<unsigned long>(b.lines);
    if (b.endOffset >= fileSize()) {
      LittleFS.remove(path_);
      LittleFS.remove(cursorPath_);
      cursor_ = 0;
      cursorLoaded_ = true;
    } else {
      writeCursor(b.endOffset);
    }
    unlock();
  }

  void clear() {
    lock();
    LittleFS.remove(path_);
    LittleFS.remove(cursorPath_);
    cursor_ = 0;
    cursorLoaded_ = true;
    unlock();
  }

 private:
  void lock() {
    if (mutex_ == nullptr) {
      static portMUX_TYPE initMux = portMUX_INITIALIZER_UNLOCKED;
      portENTER_CRITICAL(&initMux);
      if (mutex_ == nullptr) {
        mutex_ = xSemaphoreCreateMutex();
      }
      portEXIT_CRITICAL(&initMux);
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    // Callers run on the network / Influx tasks too: keep the file work out
    // of a grow-light frame (shared/flash_led_guard.h).
    FlashLedGuard::lockFlash();
  }
  void unlock() {
    FlashLedGuard::unlockFlash();
    xSemaphoreGive(mutex_);
  }

  size_t fileSize() {
    if (!LittleFS.exists(path_)) {
      return 0;
    }
    File f = LittleFS.open(path_, FILE_READ);
    if (!f) {
      return 0;
    }
    const size_t s = f.size();
    f.close();
    return s;
  }

  size_t cursor() {
    if (cursorLoaded_) {
      return cursor_;
    }
    cursor_ = 0;
    File f = LittleFS.open(cursorPath_, FILE_READ);
    if (f) {
      cursor_ = static_cast<size_t>(f.readStringUntil('\n').toInt());
      f.close();
    }
    cursorLoaded_ = true;
    return cursor_;
  }

  void writeCursor(size_t offset) {
    File f = LittleFS.open(cursorPath_, FILE_WRITE);
    if (f) {
      f.println(static_cast<unsigned long>(offset));
      f.close();
    }
    cursor_ = offset;
    cursorLoaded_ = true;
  }

  const char* path_;
  const char* cursorPath_;
  size_t maxBytes_;
  SemaphoreHandle_t mutex_ = nullptr;
  size_t cursor_ = 0;
  bool cursorLoaded_ = false;
  unsigned long appended_ = 0;
  unsigned long rejected_ = 0;
  unsigned long delivered_ = 0;
};

#endif  // SMART_GARDEN_DURABLE_QUEUE_H

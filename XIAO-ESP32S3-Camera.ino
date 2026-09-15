
#include "esp_camera.h"
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include "Adafruit_GFX.h"
#include "Adafruit_GC9A01A.h"
#include <JPEGDEC.h>

// =========================================================
// DISPLAY
// =========================================================

#define TFT_CS   D1
#define TFT_DC   D2
#define TFT_RST  D3

Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_RST);

// =========================================================
// SD CARD
// =========================================================

#define SD_CS 21

// =========================================================
// BUTTONS
// =========================================================

#define BUTTON_D4_PIN D4   // Single click = shutter
#define BUTTON_D7_PIN D7   // Double click = open latest photo
#define BUTTON_D6_PIN D6   // Double click = back to live preview

// =========================================================
// CAMERA PINS - XIAO ESP32S3 SENSE
// =========================================================

#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1

#define XCLK_GPIO_NUM     10

#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39

#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15

#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

// =========================================================
// CAMERA STATE
// =========================================================

enum CameraState {
  LIVE_PREVIEW,
  PHOTO_VIEW
};

CameraState currentState = LIVE_PREVIEW;

// =========================================================
// LAST SAVED PHOTO
// =========================================================
//
// IMPORTANT:
// We remember the exact filename immediately after saving.
// Example:
// /IMG_20.jpg
//
// This avoids repeatedly searching thousands of SD files.
// =========================================================

String lastSavedPhoto = "";

// Number of the most recently saved photo
int latestPhotoNumber = 0;

// Number of the photo currently being viewed
int currentPhotoNumber = 0;

// Exact path of the photo currently being viewed
String currentPhotoPath = "";

// Single-click events are generated only after the double-click timeout.
// This keeps the already-working double-click detection intact.
bool d7SinglePending = false;
bool d6SinglePending = false;

// =========================================================
// SD STATUS
// =========================================================

bool sdReady = false;

// =========================================================
// DOUBLE CLICK SETTINGS
// =========================================================

#define DOUBLE_CLICK_MS 1200
#define BUTTON_DEBOUNCE_MS 50

// =========================================================
// D4 STATE
// =========================================================

bool lastD4State = HIGH;

// =========================================================
// D7 STATE
// =========================================================

bool d7LastState = HIGH;
bool d7WaitingForSecond = false;
unsigned long d7FirstClickTime = 0;

// =========================================================
// D6 STATE
// =========================================================

bool d6LastRawState = HIGH;
bool d6StableState = HIGH;

bool d6WaitingForSecond = false;
unsigned long d6FirstClickTime = 0;
unsigned long d6LastDebounceTime = 0;

// =========================================================
// JPEG DECODER
// =========================================================

JPEGDEC jpeg;

// =========================================================
// JPEG DRAW CALLBACK
// =========================================================

static int drawMCU(JPEGDRAW *pDraw) {

  tft.startWrite();

  tft.setAddrWindow(
    pDraw->x,
    pDraw->y,
    pDraw->iWidth,
    pDraw->iHeight
  );

  tft.writePixels(
    (uint16_t *)pDraw->pPixels,
    pDraw->iWidth * pDraw->iHeight
  );

  tft.endWrite();

  return 1;
}

// =========================================================
// D7 DOUBLE CLICK
// =========================================================

bool checkD7DoubleClick() {

  bool currentState =
    digitalRead(BUTTON_D7_PIN);

  unsigned long now = millis();

  // -------------------------------------------------------
  // PRESS DETECTED
  // -------------------------------------------------------

  if (
    d7LastState == HIGH &&
    currentState == LOW
  ) {

    // -----------------------------------------------------
    // FIRST CLICK
    // -----------------------------------------------------

    if (!d7WaitingForSecond) {

      d7FirstClickTime = now;
      d7WaitingForSecond = true;

      Serial.println("[D7] First click");
    }

    // -----------------------------------------------------
    // SECOND CLICK
    // -----------------------------------------------------

    else {

      unsigned long elapsed =
        now - d7FirstClickTime;

      if (elapsed <= DOUBLE_CLICK_MS) {

        Serial.println("[D7] DOUBLE CLICK");

        d7WaitingForSecond = false;
        d7SinglePending = false;

        d7LastState = currentState;

        return true;
      }

      // Too slow: start a new double-click sequence
      d7FirstClickTime = now;

      Serial.println("[D7] New first click");
    }
  }

  d7LastState = currentState;

  // -------------------------------------------------------
  // TIMEOUT
  // -------------------------------------------------------

  if (
    d7WaitingForSecond &&
    (now - d7FirstClickTime > DOUBLE_CLICK_MS)
  ) {

    d7WaitingForSecond = false;

    // The first click is now confirmed as a SINGLE click.
    d7SinglePending = true;

    Serial.println("[D7] SINGLE CLICK");
  }

  return false;
}

// =========================================================
// D6 DOUBLE CLICK
// =========================================================

bool checkD6DoubleClick() {

  unsigned long now = millis();

  bool rawState =
    digitalRead(BUTTON_D6_PIN);

  // -------------------------------------------------------
  // DEBOUNCE
  // -------------------------------------------------------

  if (rawState != d6LastRawState) {

    d6LastDebounceTime = now;

    d6LastRawState = rawState;
  }

  if (
    (now - d6LastDebounceTime) >=
    BUTTON_DEBOUNCE_MS
  ) {

    if (rawState != d6StableState) {

      d6StableState = rawState;

      // ---------------------------------------------------
      // BUTTON PRESSED
      // ---------------------------------------------------

      if (d6StableState == LOW) {

        Serial.println("[D6] Press detected");

        // -------------------------------------------------
        // FIRST CLICK
        // -------------------------------------------------

        if (!d6WaitingForSecond) {

          d6FirstClickTime = now;
          d6WaitingForSecond = true;

          Serial.println("[D6] First click");
        }

        // -------------------------------------------------
        // SECOND CLICK
        // -------------------------------------------------

        else {

          unsigned long elapsed =
            now - d6FirstClickTime;

          if (elapsed <= DOUBLE_CLICK_MS) {

            Serial.println("[D6] DOUBLE CLICK");

            d6WaitingForSecond = false;
            d6SinglePending = false;

            return true;
          }

          // Too slow
          d6FirstClickTime = now;

          Serial.println("[D6] New first click");
        }
      }
    }
  }

  // -------------------------------------------------------
  // TIMEOUT
  // -------------------------------------------------------

  if (
    d6WaitingForSecond &&
    (now - d6FirstClickTime > DOUBLE_CLICK_MS)
  ) {

    d6WaitingForSecond = false;

    // The first click is now confirmed as a SINGLE click.
    d6SinglePending = true;

    Serial.println("[D6] SINGLE CLICK");
  }

  return false;
}

// =========================================================
// DISPLAY SAVED JPEG
// =========================================================

void displayPhotoFromSD(String filepath) {

  if (filepath.length() == 0) {

    Serial.println(
      "[PHOTO] No filename available"
    );

    return;
  }

  Serial.println(
    "================================"
  );

  Serial.print(
    "[PHOTO] Opening: "
  );

  Serial.println(filepath);

  Serial.println(
    "================================"
  );

  // -------------------------------------------------------
  // CHECK FILE EXISTS
  // -------------------------------------------------------

  if (!SD.exists(filepath)) {

    Serial.print(
      "[PHOTO] File does not exist: "
    );

    Serial.println(filepath);

    return;
  }

  Serial.println(
    "[PHOTO] File exists"
  );

  // -------------------------------------------------------
  // OPEN FILE
  // -------------------------------------------------------

  File photoFile =
    SD.open(
      filepath,
      FILE_READ
    );

  if (!photoFile) {

    Serial.println(
      "[PHOTO] Failed to open file"
    );

    return;
  }

  Serial.print(
    "[PHOTO] File size: "
  );

  Serial.print(
    photoFile.size()
  );

  Serial.println(
    " bytes"
  );

  Serial.println(
    "[PHOTO] File opened successfully"
  );

  // -------------------------------------------------------
  // OPEN JPEG
  // -------------------------------------------------------

  if (
    jpeg.open(
      photoFile,
      drawMCU
    )
  ) {

    Serial.printf(
      "[PHOTO] JPEG: %d x %d\n",
      jpeg.getWidth(),
      jpeg.getHeight()
    );

    // -----------------------------------------------------
    // CLEAR DISPLAY
    // -----------------------------------------------------

    tft.fillScreen(
      GC9A01A_BLACK
    );

    // -----------------------------------------------------
    // DECODE
    // -----------------------------------------------------

    Serial.println(
      "[PHOTO] Starting JPEG decode..."
    );

    int result =
      jpeg.decode(
        0,
        0,
        0
      );

    if (
      result == JPEG_SUCCESS
    ) {

      Serial.println(
        "[PHOTO] JPEG decode SUCCESS"
      );

    } else {

      Serial.print(
        "[PHOTO] JPEG decode FAILED. Error: "
      );

      Serial.println(
        jpeg.getLastError()
      );
    }

    jpeg.close();

  } else {

    Serial.print(
      "[PHOTO] JPEG open FAILED. Error: "
    );

    Serial.println(
      jpeg.getLastError()
    );
  }

  photoFile.close();

  Serial.println(
    "[PHOTO] Finished"
  );
}

// =========================================================
// SHOW PHOTO BY NUMBER
// =========================================================
//
// Uses the filename convention already used by D4:
// /IMG_1.jpg, /IMG_2.jpg, etc.
//
// =========================================================

bool showPhotoNumber(int photoNumber) {

  if (photoNumber < 1 || photoNumber > latestPhotoNumber) {
    return false;
  }

  String filename =
    "/IMG_" + String(photoNumber) + ".jpg";

  Serial.print("[PHOTO] Trying: ");
  Serial.println(filename);

  if (!SD.exists(filename)) {
    Serial.print("[PHOTO] Not found: ");
    Serial.println(filename);
    return false;
  }

  currentPhotoNumber = photoNumber;
  currentPhotoPath = filename;

  displayPhotoFromSD(filename);

  return true;
}

// =========================================================
// PREVIOUS PHOTO - D7 SINGLE CLICK
// =========================================================

void previousPhoto() {

  if (currentPhotoNumber <= 1) {
    Serial.println("[D7] Already at oldest photo");
    return;
  }

  int newNumber = currentPhotoNumber - 1;

  Serial.print("[D7] Previous photo: IMG_");
  Serial.println(newNumber);

  showPhotoNumber(newNumber);
}

// =========================================================
// NEXT PHOTO - D6 SINGLE CLICK
// =========================================================

void nextPhoto() {

  if (currentPhotoNumber >= latestPhotoNumber) {
    Serial.println("[D6] Already at latest photo");
    return;
  }

  int newNumber = currentPhotoNumber + 1;

  Serial.print("[D6] Next photo: IMG_");
  Serial.println(newNumber);

  showPhotoNumber(newNumber);
}

// =========================================================
// OPEN PHOTO VIEW
// =========================================================

void openPhotoViewer() {

  Serial.println();
  Serial.println(
    "================================"
  );

  Serial.println(
    "D7 DOUBLE CLICK"
  );

  Serial.println(
    "OPENING SAVED PHOTO"
  );

  Serial.println(
    "================================"
  );

  // -------------------------------------------------------
  // IMPORTANT:
  // Use the exact filename that was saved.
  // No 9999-file search.
  // -------------------------------------------------------

  if (
    lastSavedPhoto.length() == 0
  ) {

    Serial.println(
      "[PHOTO] No photo saved in this session"
    );

    return;
  }

  Serial.print(
    "[PHOTO] Last saved photo: "
  );

  Serial.println(
    lastSavedPhoto
  );

  // The latest filename was generated as /IMG_N.jpg.
  // Use the number already recorded when D4 saved it.
  currentPhotoNumber = latestPhotoNumber;
  currentPhotoPath = lastSavedPhoto;

  // -------------------------------------------------------
  // CHANGE STATE
  // -------------------------------------------------------

  currentState =
    PHOTO_VIEW;

  // -------------------------------------------------------
  // DISPLAY JPEG
  // -------------------------------------------------------

  displayPhotoFromSD(
    lastSavedPhoto
  );
}

// =========================================================
// RETURN TO LIVE PREVIEW
// =========================================================

void closePhotoViewer() {

  Serial.println();
  Serial.println(
    "================================"
  );

  Serial.println(
    "D6 DOUBLE CLICK"
  );

  Serial.println(
    "RETURNING TO LIVE PREVIEW"
  );

  Serial.println(
    "================================"
  );

  // -------------------------------------------------------
  // CHANGE STATE
  // -------------------------------------------------------

  currentState =
    LIVE_PREVIEW;

  currentPhotoNumber = 0;
  currentPhotoPath = "";
  d7SinglePending = false;
  d6SinglePending = false;

  // -------------------------------------------------------
  // CLEAR OLD PHOTO
  // -------------------------------------------------------

  tft.fillScreen(
    GC9A01A_BLACK
  );

  Serial.println(
    "[PHOTO] Live preview resumed"
  );
}

// =========================================================
// SETUP
// =========================================================

void setup() {

  Serial.begin(115200);

  // =======================================================
  // BUTTONS
  // =======================================================

  pinMode(
    BUTTON_D4_PIN,
    INPUT_PULLUP
  );

  pinMode(
    BUTTON_D7_PIN,
    INPUT_PULLUP
  );

  pinMode(
    BUTTON_D6_PIN,
    INPUT_PULLUP
  );

  d7LastState =
    digitalRead(
      BUTTON_D7_PIN
    );

  d6LastRawState =
    digitalRead(
      BUTTON_D6_PIN
    );

  d6StableState =
    d6LastRawState;

  lastD4State =
    digitalRead(
      BUTTON_D4_PIN
    );

  // =======================================================
  // DISPLAY
  // =======================================================

  tft.begin();

  // 270 degrees
  tft.setRotation(3);

  tft.fillScreen(
    GC9A01A_BLACK
  );

  Serial.println(
    "Display Started!"
  );

  // =======================================================
  // SD
  // =======================================================

  if (
    SD.begin(SD_CS)
  ) {

    Serial.println(
      "SD Card Initialized!"
    );

    sdReady = true;

  } else {

    Serial.println(
      "SD Card Failed!"
    );

    sdReady = false;
  }

  // =======================================================
  // CAMERA
  // =======================================================

  camera_config_t config;

  config.ledc_channel =
    LEDC_CHANNEL_0;

  config.ledc_timer =
    LEDC_TIMER_0;

  config.pin_d0 =
    Y2_GPIO_NUM;

  config.pin_d1 =
    Y3_GPIO_NUM;

  config.pin_d2 =
    Y4_GPIO_NUM;

  config.pin_d3 =
    Y5_GPIO_NUM;

  config.pin_d4 =
    Y6_GPIO_NUM;

  config.pin_d5 =
    Y7_GPIO_NUM;

  config.pin_d6 =
    Y8_GPIO_NUM;

  config.pin_d7 =
    Y9_GPIO_NUM;

  config.pin_xclk =
    XCLK_GPIO_NUM;

  config.pin_pclk =
    PCLK_GPIO_NUM;

  config.pin_vsync =
    VSYNC_GPIO_NUM;

  config.pin_href =
    HREF_GPIO_NUM;

  config.pin_sccb_sda =
    SIOD_GPIO_NUM;

  config.pin_sccb_scl =
    SIOC_GPIO_NUM;

  config.pin_pwdn =
    PWDN_GPIO_NUM;

  config.pin_reset =
    RESET_GPIO_NUM;

  config.xclk_freq_hz =
    20000000;

  config.frame_size =
    FRAMESIZE_240X240;

  config.pixel_format =
    PIXFORMAT_RGB565;

  config.grab_mode =
    CAMERA_GRAB_LATEST;

  config.fb_location =
    CAMERA_FB_IN_PSRAM;

  config.jpeg_quality =
    12;

  config.fb_count =
    2;

  // =======================================================
  // CAMERA INIT
  // =======================================================

  esp_err_t err =
    esp_camera_init(
      &config
    );

  if (
    err != ESP_OK
  ) {

    Serial.printf(
      "Camera Failed! Error: 0x%x\n",
      err
    );

    tft.fillScreen(
      GC9A01A_RED
    );

    return;
  }

  // =======================================================
  // SENSOR SETTINGS
  // =======================================================

  sensor_t *s =
    esp_camera_sensor_get();

  if (s != NULL) {

    s->set_vflip(
      s,
      1
    );

    s->set_hmirror(
      s,
      1
    );

    s->set_aec2(
      s,
      1
    );
  }

  Serial.println(
    "Camera Started!"
  );
}

// =========================================================
// LOOP
// =========================================================

void loop() {

  // =======================================================
  // D7 DOUBLE CLICK
  // =======================================================

  bool d7DoubleClick =
    checkD7DoubleClick();

  if (
    d7DoubleClick &&
    currentState == LIVE_PREVIEW
  ) {

    // Double-click D7 opens the latest saved photo.
    openPhotoViewer();

    return;
  }

  // =======================================================
  // D6 DOUBLE CLICK
  // =======================================================

  bool d6DoubleClick =
    checkD6DoubleClick();

  if (
    d6DoubleClick &&
    currentState == PHOTO_VIEW
  ) {

    // Double-click D6 returns to live preview.
    closePhotoViewer();

    return;
  }

  // =======================================================
  // PHOTO VIEW MODE
  // =======================================================
  //
  // D7 SINGLE  -> previous photo
  // D6 SINGLE  -> next photo
  // D6 DOUBLE  -> live preview
  //
  // The single-click action happens only after the
  // double-click timeout, so it cannot steal a double click.
  // =======================================================

  if (
    currentState == PHOTO_VIEW
  ) {

    if (d7SinglePending) {

      d7SinglePending = false;

      previousPhoto();

      return;
    }

    if (d6SinglePending) {

      d6SinglePending = false;

      nextPhoto();

      return;
    }

    // Do NOT grab camera frames while viewing a photo.
    delay(5);

    return;
  }

  // =======================================================
  // LIVE PREVIEW MODE
  // =======================================================

  // A single D7/D6 click has no action in live preview.
  // If one occurs, simply consume the pending event.
  d7SinglePending = false;
  d6SinglePending = false;

  // =======================================================
  // D4 SINGLE CLICK
  // =======================================================

  bool currentD4State =
    digitalRead(
      BUTTON_D4_PIN
    );

  bool d4IsPressed =
    (
      currentD4State == LOW &&
      lastD4State == HIGH
    );

  lastD4State =
    currentD4State;

  // =======================================================
  // GET CAMERA FRAME
  // =======================================================

  camera_fb_t *fb =
    esp_camera_fb_get();

  if (!fb) {

    return;
  }

  // =======================================================
  // LIVE PREVIEW
  // =======================================================

  // -------------------------------------------------------
  // BYTE SWAP FOR DISPLAY
  // -------------------------------------------------------

  for (
    uint32_t i = 0;
    i < fb->len;
    i += 2
  ) {

    uint8_t temp =
      fb->buf[i];

    fb->buf[i] =
      fb->buf[i + 1];

    fb->buf[i + 1] =
      temp;
  }

  // -------------------------------------------------------
  // DISPLAY LIVE FRAME
  // -------------------------------------------------------

  tft.startWrite();

  tft.setAddrWindow(
    0,
    0,
    240,
    240
  );

  tft.writePixels(
    (uint16_t *)fb->buf,
    240 * 240
  );

  tft.endWrite();

  // =======================================================
  // D4 SHUTTER
  // =======================================================

  if (
    d4IsPressed
  ) {

    Serial.println();
    Serial.println(
      "================================"
    );

    Serial.println(
      "D4 SHUTTER"
    );

    Serial.println(
      "================================"
    );

    // -----------------------------------------------------
    // WHITE FLASH
    // -----------------------------------------------------

    tft.fillScreen(
      GC9A01A_WHITE
    );

    delay(50);

    // -----------------------------------------------------
    // FROZEN PHOTO
    // -----------------------------------------------------

    tft.startWrite();

    tft.setAddrWindow(
      0,
      0,
      240,
      240
    );

    tft.writePixels(
      (uint16_t *)fb->buf,
      240 * 240
    );

    tft.endWrite();

    // -----------------------------------------------------
    // SCANNER ANIMATION
    // -----------------------------------------------------

    for (
      int y = 0;
      y < 240;
      y += 4
    ) {

      tft.drawFastHLine(
        0,
        y,
        240,
        GC9A01A_CYAN
      );

      tft.drawFastHLine(
        0,
        y + 1,
        240,
        0x07FF
      );

      delay(8);

      // ---------------------------------------------------
      // RESTORE IMAGE BEHIND SCANNER
      // ---------------------------------------------------

      if (
        y > 8
      ) {

        tft.startWrite();

        tft.setAddrWindow(
          0,
          y - 8,
          240,
          8
        );

        int offset_pixels =
          (y - 8) * 240;

        tft.writePixels(
          (uint16_t *)(
            fb->buf +
            (offset_pixels * 2)
          ),
          240 * 8
        );

        tft.endWrite();
      }
    }

    // -----------------------------------------------------
    // RESTORE FULL PHOTO
    // -----------------------------------------------------

    tft.startWrite();

    tft.setAddrWindow(
      0,
      0,
      240,
      240
    );

    tft.writePixels(
      (uint16_t *)fb->buf,
      240 * 240
    );

    tft.endWrite();

    // =====================================================
    // SAVE TO SD
    // =====================================================

    if (
      sdReady
    ) {

      Serial.println(
        "Processing photo..."
      );

      // ---------------------------------------------------
      // UNSWAP BYTES
      // ---------------------------------------------------

      for (
        uint32_t i = 0;
        i < fb->len;
        i += 2
      ) {

        uint8_t temp =
          fb->buf[i];

        fb->buf[i] =
          fb->buf[i + 1];

        fb->buf[i + 1] =
          temp;
      }

      // ---------------------------------------------------
      // RGB565 -> JPEG
      // ---------------------------------------------------

      uint8_t *jpeg_buf =
        NULL;

      size_t jpeg_len =
        0;

      bool converted =
        fmt2jpg(
          fb->buf,
          fb->len,
          fb->width,
          fb->height,
          PIXFORMAT_RGB565,
          80,
          &jpeg_buf,
          &jpeg_len
        );

      if (
        converted
      ) {

        // -----------------------------------------------
        // FIND NEXT FREE FILENAME
        // -----------------------------------------------

        int fileNum = 1;

        String filename;

        while (
          true
        ) {

          filename =
            "/IMG_" +
            String(fileNum) +
            ".jpg";

          if (
            !SD.exists(filename)
          ) {

            break;
          }

          fileNum++;
        }

        // -----------------------------------------------
        // SAVE JPEG
        // -----------------------------------------------

        File file =
          SD.open(
            filename,
            FILE_WRITE
          );

        if (
          file
        ) {

          size_t written =
            file.write(
              jpeg_buf,
              jpeg_len
            );

          file.close();

          if (
            written == jpeg_len
          ) {

            // ---------------------------------------------
            // REMEMBER LATEST PHOTO
            // ---------------------------------------------

            lastSavedPhoto =
              filename;

            latestPhotoNumber =
              fileNum;

            Serial.print(
              "Saved: "
            );

            Serial.println(
              lastSavedPhoto
            );

            Serial.print(
              "Latest photo number: "
            );

            Serial.println(
              latestPhotoNumber
            );

          } else {

            Serial.println(
              "SD write incomplete"
            );
          }

        } else {

          Serial.println(
            "Failed to write file"
          );
        }

        free(
          jpeg_buf
        );

      } else {

        Serial.println(
          "JPEG conversion failed"
        );
      }

    } else {

      Serial.println(
        "Warning: No SD card detected!"
      );
    }

    // -----------------------------------------------------
    // KEEP FROZEN PHOTO
    // -----------------------------------------------------

    delay(1000);
  }

  // =======================================================
  // RETURN CAMERA BUFFER
  // =======================================================

  esp_camera_fb_return(
    fb
  );
}

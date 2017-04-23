// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
//
// This code is public domain
// (but note, once linked against the led-matrix library, this is
// covered by the GPL v2)
//
// This is a grab-bag of various demos and not very readable.
#include "led-matrix.h"
#include "threaded-canvas-manipulator.h"
#include "transformer.h"
#include "graphics.h"

#include <assert.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>

#include <Magick++.h>
#include <magick/image.h>

using std::min;
using std::max;

#define TERM_ERR  "\033[1;31m"
#define TERM_NORM "\033[0m"

using namespace rgb_matrix;

volatile bool interrupt_received = false;
static void InterruptHandler(int signo) {
  interrupt_received = true;
}

/*
 * The following are demo image generators. They all use the utility
 * class ThreadedCanvasManipulator to generate new frames.
 */

class ImageScroller : public ThreadedCanvasManipulator {
public:
  // Scroll image with "scroll_jumps" pixels every "scroll_ms" milliseconds.
  // If "scroll_ms" is negative, don't do any scrolling.
  ImageScroller(RGBMatrix *m, int scroll_jumps, int scroll_ms = 30, bool scroll_vertical = false, int loop_count = 0)
    : ThreadedCanvasManipulator(m), scroll_jumps_(scroll_jumps),
      scroll_ms_(scroll_ms),
      scroll_vertical_(scroll_vertical),
      scroll_position_(0),
      loop_count_(loop_count),
      matrix_(m) {
    offscreen_ = matrix_->CreateFrameCanvas();
  }

  virtual ~ImageScroller() {
    Stop();
    WaitStopped();   // only now it is safe to delete our instance variables.
  }

  bool LoadImage(const char *filename) {
    std::vector<Magick::Image> frames;
    try {
        readImages(&frames, filename);
    } catch (std::exception& e) {
        fprintf(stderr, "Trouble loading %s (%s)\n", filename, e.what());
        return false;
    }
    if (frames.size() != 1) {
        fprintf(stderr, "Image must have exactly one frame.");
        return false;
    }

    Magick::Image &img = frames[0];

    Pixel *new_image = new Pixel [ img.rows() * img.columns()];
    assert(sizeof(Pixel) == 3);   // we make that assumption.

    for (size_t y = 0; y < img.rows(); ++y) {
      for (size_t x = 0; x < img.columns(); ++x) {
        const Magick::Color &c = img.pixelColor(x, y);
        if (c.alphaQuantum() < 256) {
          size_t pixelIndex = (y * img.columns()) + x;
          new_image[pixelIndex].red = ScaleQuantumToChar(c.redQuantum());
          new_image[pixelIndex].green = ScaleQuantumToChar(c.greenQuantum());
          new_image[pixelIndex].blue = ScaleQuantumToChar(c.blueQuantum());
        }
      }
    }

    fprintf(stderr, "Read image '%s' with %dx%d\n", filename,
        img.rows(), img.columns());

    if (scroll_vertical_) {
      if (scroll_jumps_ < 0) {
        scroll_position_ = img.rows() - matrix_->height();
        if (scroll_position_ < 0) scroll_position_ = img.rows();
      } else {
        scroll_position_ = 0;
      }
    } else {
      if (scroll_jumps_ < 0) {
        scroll_position_ = img.columns() - matrix_->width();
        if (scroll_position_ < 0) scroll_position_ = img.columns();
      } else {
        scroll_position_ = 0;
      }
    }

    MutexLock l(&mutex_new_image_);
    new_image_.Delete();  // in case we reload faster than is picked up
    new_image_.image = new_image;
    new_image_.width = img.columns();
    new_image_.height = img.rows();

    return true;
  }

  void Run() {
    const int screen_height = offscreen_->height();
    const int screen_width = offscreen_->width();
    while (running() && !interrupt_received && (loop_count_ != 0)) {
      {
        MutexLock l(&mutex_new_image_);
        if (new_image_.IsValid()) {
          current_image_.Delete();
          current_image_ = new_image_;
          new_image_.Reset();
        }
      }
      if (!current_image_.IsValid()) {
        usleep(100 * 1000);
        continue;
      }

      if (scroll_vertical_) {
        for (int y = 0; y < screen_height; ++y) {
          int ysrc = (scroll_position_ + y) % current_image_.height;
          for (int x = 0; x < screen_width; ++x) {
            const Pixel &p = current_image_.getPixel(x, ysrc);
            offscreen_->SetPixel(x, y, p.red, p.green, p.blue);
          }
        }
      } else {
        for (int x = 0; x < screen_width; ++x) {
          int xsrc = (scroll_position_ + x) % current_image_.width;
          for (int y = 0; y < screen_height; ++y) {
            const Pixel &p = current_image_.getPixel(xsrc, y);
            offscreen_->SetPixel(x, y, p.red, p.green, p.blue);
          }
        }
      }
      offscreen_ = matrix_->SwapOnVSync(offscreen_);
      scroll_position_ += scroll_jumps_;

      const int *image_size_units = NULL;
      const int *offscreen_size_units = NULL;

      if (scroll_vertical_) {
        image_size_units = &(current_image_.height);
        offscreen_size_units = &(screen_height);
      } else {
        image_size_units = &(current_image_.width);
        offscreen_size_units = &(screen_width);
      }

      if (scroll_jumps_ < 0) {
        if (scroll_position_ < 0) {
          scroll_position_ = *image_size_units - 1;
          loop_count_--;
          if (loop_count_ < 0) loop_count_ = -1;
        }
      } else {
        int32_t scroll_position_limit = *image_size_units;
        if (loop_count_ == 1) scroll_position_limit -= *offscreen_size_units;
        if (scroll_position_ >= scroll_position_limit) {
          scroll_position_ = 0;
          loop_count_--;
          if (loop_count_ < 0) loop_count_ = -1;
        }
      }

      if (scroll_ms_ <= 0) {
        // No scrolling. We don't need the image anymore.
        current_image_.Delete();
      } else {
        usleep(scroll_ms_ * 1000);
      }
    }
  }

private:
  struct Pixel {
    Pixel() : red(0), green(0), blue(0){}
    uint8_t red;
    uint8_t green;
    uint8_t blue;
  };

  struct Image {
    Image() : width(-1), height(-1), image(NULL) {}
    ~Image() { Delete(); }
    void Delete() { delete [] image; Reset(); }
    void Reset() { image = NULL; width = -1; height = -1; }
    inline bool IsValid() { return image && height > 0 && width > 0; }
    const Pixel &getPixel(int x, int y) {
      static Pixel black;
      if (x < 0 || x >= width || y < 0 || y >= height) return black;
      return image[x + width * y];
    }

    int width;
    int height;
    Pixel *image;
  };

  // Read line, skip comments.
  char *ReadLine(FILE *f, char *buffer, size_t len) {
    char *result;
    do {
      result = fgets(buffer, len, f);
    } while (result != NULL && result[0] == '#');
    return result;
  }

  const int scroll_jumps_;
  const int scroll_ms_;
  const bool scroll_vertical_;

  // Current image is only manipulated in our thread.
  Image current_image_;

  // New image can be loaded from another thread, then taken over in main thread
  Mutex mutex_new_image_;
  Image new_image_;

  int32_t scroll_position_;
  int loop_count_; // < 0: loop forever, otherwise loops to run

  RGBMatrix* matrix_;
  FrameCanvas* offscreen_;
};


static int usage(const char *progname) {
  fprintf(stderr, "usage: %s <options> <image filename>\n",
          progname);
  fprintf(stderr, "Options:\n");
  fprintf(stderr,
          "\t-L                        : Large display, in which each chain is 'folded down'\n"
          "\t                            in the middle in an U-arrangement to get more vertical space.\n"
          "\t-R <rotation>             : Sets the rotation of matrix. "
          "Allowed: 0, 90, 180, 270. Default: 0.\n"
          "\t-l<loop-count>            : Number of scroll loops (default -1=forever).\n"
          "\t-m <scoll_msecs>          : Pause between two scroll steps in msec.\n"
          "\t-j <scroll jumps>         : Pixels to shift at each scroll step (-x ... -1, 1 ... x).\n"
          "\t-t <seconds>              : Run for these number of seconds, then exit.\n"
          "\t-v                        : Scroll vertically instead of horizontal.\n");


  rgb_matrix::PrintMatrixFlags(stderr);

  return 1;
}

int main(int argc, char *argv[]) {
  int runtime_seconds = -1;
  int scroll_ms = 30;
  int scroll_jumps = 1;
  int scoll_loops = -1;
  int rotation = 0;
  bool large_display = false;
  bool scroll_vertical = false;

  const char *image_filename = NULL;
  RGBMatrix::Options matrix_options;
  rgb_matrix::RuntimeOptions runtime_opt;

  Magick::InitializeMagick(*argv);

  // These are the defaults when no command-line flags are given.
  matrix_options.rows = 32;
  matrix_options.chain_length = 1;
  matrix_options.parallel = 1;

  // First things first: extract the command line flags that contain
  // relevant matrix options.
  if (!ParseOptionsFromFlags(&argc, &argv, &matrix_options, &runtime_opt)) {
    return usage(argv[0]);
  }

  int opt;
  while ((opt = getopt(argc, argv, "dt:r:P:c:p:b:l:m:LR:j:v")) != -1) {
    switch (opt) {
    case 't':
      runtime_seconds = atoi(optarg);
      break;

    case 'm':
      scroll_ms = atoi(optarg);
      break;

    case 'l':
      scoll_loops = atoi(optarg);
      break;

    case 'R':
      rotation = atoi(optarg);
      break;

    case 'L':
      if (matrix_options.chain_length == 1) {
        // If this is still default, force the 64x64 arrangement.
        matrix_options.chain_length = 4;
      }
      large_display = true;
      break;

      // These used to be options we understood, but deprecated now. Accept
      // but don't mention in usage()
    case 'd':
      runtime_opt.daemon = 1;
      break;

    case 'r':
      matrix_options.rows = atoi(optarg);
      break;

    case 'P':
      matrix_options.parallel = atoi(optarg);
      break;

    case 'c':
      matrix_options.chain_length = atoi(optarg);
      break;

    case 'p':
      matrix_options.pwm_bits = atoi(optarg);
      break;

    case 'b':
      matrix_options.brightness = atoi(optarg);
      break;

    case 'j':
      scroll_jumps = atoi(optarg);
      break;

    case 'v':
      scroll_vertical = true;
      break;

    default: /* '?' */
      return usage(argv[0]);
    }
  }

  if (optind < argc) {
    image_filename = argv[optind];
  }

  if (!image_filename) {
    return usage(argv[0]);
  }

  if (rotation % 90 != 0) {
    fprintf(stderr, TERM_ERR "Rotation %d not allowed! "
            "Only 0, 90, 180 and 270 are possible.\n" TERM_NORM, rotation);
    return 1;
  }

  RGBMatrix *matrix = CreateMatrixFromOptions(matrix_options, runtime_opt);
  if (matrix == NULL)
    return 1;

  if (large_display) {
    // Mapping the coordinates of a 32x128 display mapped to a square of 64x64.
    // Or any other U-arrangement.
    matrix->ApplyStaticTransformer(UArrangementTransformer(
                                     matrix_options.parallel));
  }

  if (rotation > 0) {
    matrix->ApplyStaticTransformer(RotateTransformer(rotation));
  }

  printf("Size: %dx%d. Hardware gpio mapping: %s\n",
         matrix->width(), matrix->height(), matrix_options.hardware_mapping);

  Canvas *canvas = matrix;

  // The ThreadedCanvasManipulator objects are filling
  // the matrix continuously.
  ThreadedCanvasManipulator *image_gen = NULL;
  ImageScroller *scroller = new ImageScroller(matrix, scroll_jumps, scroll_ms, scroll_vertical, scoll_loops);
  if (!scroller->LoadImage(image_filename))
    return 1;
  image_gen = scroller;

  // Set up an interrupt handler to be able to stop animations while they go
  // on. Note, each demo tests for while (running() && !interrupt_received) {},
  // so they exit as soon as they get a signal.
  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);

  // Image generating demo is crated. Now start the thread.
  image_gen->Start();

  // Now, the image generation runs in the background. We can do arbitrary
  // things here in parallel. In this demo, we're essentially just
  // waiting for one of the conditions to exit.
  if (runtime_seconds > 0) {
    sleep(runtime_seconds);
  } else {
    // The
    printf("Press <CTRL-C> to exit and reset LEDs\n");
    image_gen->WaitStopped();
  }

  // Stop image generating thread. The delete triggers
  delete image_gen;
  delete canvas;

  printf("\%s. Exiting.\n",
         interrupt_received ? "Received CTRL-C" : "Timeout reached");
  return 0;
}

#include "camera/v4l2_camera.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <turbojpeg.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>

namespace lhc {

namespace {

constexpr int kBufferCount = 4;

int xioctl(int fd, unsigned long request, void* arg) {
  int r;
  do {
    r = ioctl(fd, request, arg);
  } while (r == -1 && errno == EINTR);
  return r;
}

int64_t monotonicNowUs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

std::string errnoText(const std::string& what) { return what + ": " + std::strerror(errno); }

}  // namespace

V4l2Camera::~V4l2Camera() {
  if (streaming_) {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(fd_, VIDIOC_STREAMOFF, &type);
  }
  for (const Buffer& b : buffers_) {
    if (b.start != nullptr) {
      munmap(b.start, b.length);
    }
  }
  if (tj_ != nullptr) {
    tjDestroy(tj_);
  }
  if (fd_ >= 0) {
    close(fd_);
  }
}

std::unique_ptr<V4l2Camera> V4l2Camera::open(const std::string& path, std::string& error) {
  std::unique_ptr<V4l2Camera> camera(new V4l2Camera());
  if (!camera->init(path, error)) {
    return nullptr;
  }
  return camera;
}

bool V4l2Camera::init(const std::string& path, std::string& error) {
  fd_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) {
    error = errnoText("open " + path);
    return false;
  }

  v4l2_capability cap{};
  if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
    error = errnoText("VIDIOC_QUERYCAP");
    return false;
  }
  const uint32_t caps =
      (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
  if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
    error = path + " cannot stream video capture";
    return false;
  }
  return chooseFormat(error) && startStreaming(error);
}

bool V4l2Camera::chooseFormat(std::string& error) {
  v4l2_format current{};
  current.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(fd_, VIDIOC_G_FMT, &current) < 0) {
    error = errnoText("VIDIOC_G_FMT");
    return false;
  }

  // GREY is what the IR sensor in this project's target laptops offers; the
  // others cover cameras that only expose a colour-style stream.
  for (uint32_t wanted : {V4L2_PIX_FMT_GREY, V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_MJPEG}) {
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = current.fmt.pix.width;
    fmt.fmt.pix.height = current.fmt.pix.height;
    fmt.fmt.pix.pixelformat = wanted;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    const int rc = xioctl(fd_, VIDIOC_S_FMT, &fmt);
    if (rc < 0 && errno == EBUSY) {
      error = "camera busy (another process is streaming from it)";
      return false;
    }
    if (rc == 0 && fmt.fmt.pix.pixelformat == wanted) {
      pixel_format_ = wanted;
      width_ = static_cast<int>(fmt.fmt.pix.width);
      height_ = static_cast<int>(fmt.fmt.pix.height);
      stride_ = fmt.fmt.pix.bytesperline;
      return true;
    }
  }
  error = "camera offers none of GREY, YUYV, MJPEG";
  return false;
}

bool V4l2Camera::startStreaming(std::string& error) {
  v4l2_requestbuffers req{};
  req.count = kBufferCount;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
    error = errnoText("VIDIOC_REQBUFS");
    return false;
  }

  for (uint32_t i = 0; i < req.count; ++i) {
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
      error = errnoText("VIDIOC_QUERYBUF");
      return false;
    }
    void* start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
    if (start == MAP_FAILED) {
      error = errnoText("mmap");
      return false;
    }
    buffers_.push_back({start, buf.length});
    if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
      error = errnoText("VIDIOC_QBUF");
      return false;
    }
  }

  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
    error = errnoText("VIDIOC_STREAMON");
    return false;
  }
  streaming_ = true;

  if (pixel_format_ == V4L2_PIX_FMT_MJPEG) {
    tj_ = tjInitDecompress();
    if (tj_ == nullptr) {
      error = "cannot initialise libturbojpeg";
      return false;
    }
  }
  return true;
}

bool V4l2Camera::convert(const Buffer& buffer, size_t bytes_used, GreyImage& out) {
  const auto* src = static_cast<const uint8_t*>(buffer.start);
  const size_t w = static_cast<size_t>(width_);
  const size_t h = static_cast<size_t>(height_);
  bytes_used = std::min(bytes_used, buffer.length);

  switch (pixel_format_) {
    case V4L2_PIX_FMT_GREY: {
      const size_t stride = std::max(stride_, w);
      if (bytes_used < stride * (h - 1) + w) {
        return false;
      }
      out = GreyImage(width_, height_);
      for (size_t y = 0; y < h; ++y) {
        std::memcpy(&out.data[y * w], src + y * stride, w);
      }
      return true;
    }
    case V4L2_PIX_FMT_YUYV: {
      const size_t stride = std::max(stride_, w * 2);
      if (bytes_used < stride * (h - 1) + w * 2) {
        return false;
      }
      out = GreyImage(width_, height_);
      for (size_t y = 0; y < h; ++y) {
        const uint8_t* row = src + y * stride;
        for (size_t x = 0; x < w; ++x) {
          out.data[y * w + x] = row[x * 2];  // Y of each YUYV pair
        }
      }
      return true;
    }
    case V4L2_PIX_FMT_MJPEG: {
      int jw = 0, jh = 0, subsamp = 0, colorspace = 0;
      auto* jpeg = const_cast<unsigned char*>(src);
      if (tjDecompressHeader3(tj_, jpeg, static_cast<unsigned long>(bytes_used), &jw, &jh, &subsamp,
                              &colorspace) != 0 ||
          jw <= 0 || jh <= 0) {
        return false;
      }
      out = GreyImage(jw, jh);
      return tjDecompress2(tj_, jpeg, static_cast<unsigned long>(bytes_used), out.data.data(), jw,
                           0, jh, TJPF_GRAY, TJFLAG_FASTDCT) == 0;
    }
    default:
      return false;
  }
}

ReadStatus V4l2Camera::read(Frame& out, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));

  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::microseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
      return ReadStatus::kTimeout;
    }

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd_, &fds);
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(left.count() / 1000000);
    tv.tv_usec = static_cast<suseconds_t>(left.count() % 1000000);
    const int ready = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      return ReadStatus::kError;
    }
    if (ready == 0) {
      return ReadStatus::kTimeout;
    }

    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
      if (errno == EAGAIN) {
        continue;
      }
      return ReadStatus::kError;
    }

    const bool damaged = (buf.flags & V4L2_BUF_FLAG_ERROR) != 0;
    const bool converted = !damaged && convert(buffers_[buf.index], buf.bytesused, out.image);
    // Hand the buffer straight back so the driver never runs out.
    if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
      return ReadStatus::kError;
    }
    if (!converted) {
      continue;  // a damaged frame is not worth failing for; wait for the next one
    }

    out.timestamp_us = static_cast<int64_t>(buf.timestamp.tv_sec) * 1000000 + buf.timestamp.tv_usec;
    if (out.timestamp_us == 0) {
      out.timestamp_us = monotonicNowUs();
    }
    return ReadStatus::kOk;
  }
}

}  // namespace lhc

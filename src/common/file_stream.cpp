#include "common/file_stream.h"

#include <streambuf>
#include <string>
#include <vector>
#ifdef _MSC_VER
#include <io.h>
#include <windows.h>
#include <fcntl.h>
#include <stdlib.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace marian {
namespace io {

///////////////////////////////////////////////////////////////////////////////////////////////
InputFileStream::InputFileStream(const std::string &file)
    : std::istream(NULL), file_(file), streamBuf1_(NULL), streamBuf2_(NULL) {
  ABORT_IF(!marian::filesystem::exists(file_), "File '{}' does not exist", file);

  std::filebuf *fileBuf = new std::filebuf();
  streamBuf1_ = fileBuf->open(file.c_str(), std::ios::in);
  ABORT_IF(!streamBuf1_, "File can't be opened", file);
  assert(fileBuf == streamBuf1_);

  if(file_.extension() == marian::filesystem::Path(".gz")) {
    streamBuf2_ = new zstr::istreambuf(streamBuf1_);
    this->init(streamBuf2_);
  } else {
    this->init(streamBuf1_);
  }
}

InputFileStream::~InputFileStream() {
  delete streamBuf2_;
  delete streamBuf1_;
}

bool InputFileStream::empty() {
  return this->peek() == std::ifstream::traits_type::eof();
}

void InputFileStream::setbufsize(size_t size) {
  rdbuf()->pubsetbuf(0, 0);
  readBuf_.resize(size);
  rdbuf()->pubsetbuf(readBuf_.data(), readBuf_.size());
}

std::string InputFileStream::getFileName() const {
  return file_.string();
}

// wrapper around std::getline() that handles Windows input files with extra CR
// chars at the line end
std::istream &getline(std::istream &in, std::string &line) {
  std::getline(in, line);
  // bad() seems to be correct here. Should not abort on EOF.
  ABORT_IF(in.bad(), "Error reading from stream");
  // strip terminal CR if present
  if(in && !line.empty() && line.back() == in.widen('\r'))
    line.pop_back();
  return in;
}
///////////////////////////////////////////////////////////////////////////////////////////////
OutputFileStream::OutputFileStream(const std::string &file)
    : std::ostream(NULL), file_(file), streamBuf1_(NULL), streamBuf2_(NULL) {
  std::filebuf *fileBuf = new std::filebuf();
  streamBuf1_ = fileBuf->open(file.c_str(), std::ios::out | std::ios_base::binary);
  ABORT_IF(!streamBuf1_, "File can't be opened", file);
  assert(fileBuf == streamBuf1_);

  if(file_.extension() == marian::filesystem::Path(".gz")) {
    streamBuf2_ = new zstr::ostreambuf(streamBuf1_);
    this->init(streamBuf2_);
  } else {
    this->init(streamBuf1_);
  }
}

OutputFileStream::OutputFileStream()
    : std::ostream(NULL), streamBuf1_(NULL), streamBuf2_(NULL) {}

OutputFileStream::~OutputFileStream() {
  this->flush();
  delete streamBuf2_;
  delete streamBuf1_;
}

///////////////////////////////////////////////////////////////////////////////////////////////
TemporaryFile::TemporaryFile(const std::string &base, bool earlyUnlink)
    : OutputFileStream(), unlink_(earlyUnlink) {
  std::string baseTemp(base);
  NormalizeTempPrefix(baseTemp);
  MakeTemp(baseTemp);

  inSteam_ = UPtr<io::InputFileStream>(new io::InputFileStream(file_.string()));
  if(unlink_) {
    ABORT_IF(remove(file_.string().c_str()), "Error while deleting '{}'", file_.string());
  }
}

TemporaryFile::~TemporaryFile() {
  if(!unlink_) {
// suppress 'throw will always call terminate() [-Wterminate]'
#if __GNUC__ >= 7
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wterminate"
#endif
    ABORT_IF(remove(file_.string().c_str()), "Error while deleting '{}'", file_.string());
#if __GNUC__ >= 7
#pragma GCC diagnostic pop
#endif
  }
}

void TemporaryFile::NormalizeTempPrefix(std::string &base) const {
  if(base.empty())
    return;

#ifdef _MSC_VER
  if(base.substr(0, 4) == "/tmp")
    base = getenv("TMP");
#else
  if(base[base.size() - 1] == '/')
    return;
  struct stat sb;
  // It's fine for it to not exist.
  if(stat(base.c_str(), &sb) == -1)
    return;
  if(S_ISDIR(sb.st_mode))
    base += '/';
#endif
}
void TemporaryFile::MakeTemp(const std::string &base) {
#ifdef _MSC_VER
  char *name = tempnam(base.c_str(), "marian.");
  ABORT_IF(name == NULL, "Error while making a temporary based on '{}'", base);

  int oflag = _O_RDWR | _O_CREAT | _O_EXCL;
  if(unlink_)
    oflag |= _O_TEMPORARY;

  int fd = open(name, oflag, _S_IREAD | _S_IWRITE);
  ABORT_IF(fd == -1, "Error while making a temporary based on '{}'", base);

#else
  // create temp file
  std::string name(base);
  name += "marian.XXXXXX";
  name.push_back(0);
  int fd = mkstemp(&name[0]);
  ABORT_IF(fd == -1, "Error creating temp file {}", name);

  file_ = name;
#endif

  // open again with c++
  std::filebuf *fileBuf = new std::filebuf();
  streamBuf1_ = fileBuf->open(name, std::ios::out | std::ios_base::binary);
  ABORT_IF(!streamBuf1_, "File can't be temp opened", name);
  assert(fileBuf == streamBuf1_);

  this->init(streamBuf1_);

  // close original file descriptor
  ABORT_IF(close(fd), "Can't close file descriptor", name);

#ifdef _MSC_VER
  free(name);
#endif
}

UPtr<InputFileStream> TemporaryFile::getInputStream() {
  return std::move(inSteam_);
}

std::string TemporaryFile::getFileName() const {
  return file_.string();
}

}  // namespace io
}  // namespace marian

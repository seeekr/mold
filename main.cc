#include "chibild.h"

#include <iostream>

using llvm::file_magic;
using llvm::object::Archive;
using llvm::opt::InputArgList;

Config config;

//
// Command-line option processing
//

enum {
  OPT_INVALID = 0,
#define OPTION(_1, _2, ID, _4, _5, _6, _7, _8, _9, _10, _11, _12) OPT_##ID,
#include "options.inc"
#undef OPTION
};

// Create prefix string literals used in Options.td
#define PREFIX(NAME, VALUE) const char *const NAME[] = VALUE;
#include "options.inc"
#undef PREFIX

// Create table mapping all options defined in Options.td
static const llvm::opt::OptTable::Info opt_info[] = {
#define OPTION(X1, X2, ID, KIND, GROUP, ALIAS, X7, X8, X9, X10, X11, X12)      \
  {X1, X2, X10,         X11,         OPT_##ID, llvm::opt::Option::KIND##Class, \
   X9, X8, OPT_##GROUP, OPT_##ALIAS, X7,       X12},
#include "options.inc"
#undef OPTION
};

class MyOptTable : llvm::opt::OptTable {
public:
  MyOptTable() : OptTable(opt_info) {}
  InputArgList parse(int argc, char **argv);
};

InputArgList MyOptTable::parse(int argc, char **argv) {
  unsigned missingIndex;
  unsigned missingCount;
  SmallVector<const char *, 256> vec(argv, argv + argc);

  InputArgList args = this->ParseArgs(vec, missingIndex, missingCount);
  if (missingCount)
    error(Twine(args.getArgString(missingIndex)) + ": missing argument");

  for (auto *arg : args.filtered(OPT_UNKNOWN))
    error("unknown argument '" + arg->getAsString(args) + "'");
  return args;
}

//
// Main
//

static std::vector<MemoryBufferRef> get_archive_members(MemoryBufferRef mb) {
  std::unique_ptr<Archive> file =
    CHECK(Archive::create(mb), mb.getBufferIdentifier() + ": failed to parse archive");

  std::vector<MemoryBufferRef> vec;

  Error err = Error::success();

  for (const Archive::Child &c : file->children(err)) {
    MemoryBufferRef mbref =
        CHECK(c.getMemoryBufferRef(),
              mb.getBufferIdentifier() +
                  ": could not get the buffer for a child of the archive");
    vec.push_back(mbref);
  }

  if (err)
    error(mb.getBufferIdentifier() + ": Archive::children failed: " +
          toString(std::move(err)));

  file.release(); // leak
  return vec;
}

static std::vector<ObjectFile *> read_file(StringRef path) {
  MemoryBufferRef mb = readFile(path);
  std::vector<ObjectFile *> vec;

  switch (identify_magic(mb.getBuffer())) {
  case file_magic::archive:
    for (MemoryBufferRef member : get_archive_members(mb))
      vec.push_back(new ObjectFile(member, path));
    break;
  case file_magic::elf_relocatable:
    vec.push_back(new ObjectFile(mb, ""));
    break;
  default:
    error(path + ": unknown file type");
  }
  return vec;
}

template<typename T, typename Callable>
static void for_each(T &arr, Callable callback) {
  tbb::parallel_for_each(arr.begin(), arr.end(), callback);
}

int main(int argc, char **argv) {
  // Parse command line options
  MyOptTable opt_table;
  InputArgList args = opt_table.parse(argc - 1, argv + 1);

  if (auto *arg = args.getLastArg(OPT_o))
    config.output = arg->getValue();
  else
    error("-o option is missing");

  std::vector<ObjectFile *> files;

  llvm::Timer open_timer("opne", "open");
  llvm::Timer parse_timer("parse", "parse");
  llvm::Timer add_symbols_timer("add_symbols", "add_symbols");
  llvm::Timer comdat_timer("comdat", "comdat");
  llvm::Timer output_section_timer("output_section", "output_section");
  llvm::Timer file_offset_timer("file_offset", "file_offset");

  // Open input files
  open_timer.startTimer();
  for (auto *arg : args)
    if (arg->getOption().getID() == OPT_INPUT)
      for (ObjectFile *file : read_file(arg->getValue()))
        files.push_back(file);
  open_timer.stopTimer();

  // Parse input files
  parse_timer.startTimer();
  for_each(files, [](ObjectFile *file) { file->parse(); });
  parse_timer.stopTimer();

  // Set priorities to files
  for (int i = 0; i < files.size(); i++)
    files[i]->priority = files[i]->is_in_archive() ? i + (1 << 31) : i;

  // Resolve symbols
  add_symbols_timer.startTimer();
  for_each(files, [](ObjectFile *file) { file->register_defined_symbols(); });
  for_each(files, [](ObjectFile *file) { file->register_undefined_symbols(); });
  add_symbols_timer.stopTimer();

  // Eliminate unused archive members.
  files.erase(std::remove_if(files.begin(), files.end(),
                             [](ObjectFile *file){ return !file->is_alive; }),
              files.end());

  // Eliminate duplicate comdat groups.
  comdat_timer.startTimer();
  for (ObjectFile *file : files)
    file->eliminate_duplicate_comdat_groups();
  comdat_timer.stopTimer();

  // Bin input sections into output sections
  std::vector<OutputSection *> output_sections;

  output_section_timer.startTimer();
  for (ObjectFile *file : files) {
    for (InputSection *isec : file->sections) {
      if (!isec)
        continue;

      OutputSection *osec = isec->output_section;
      if (osec->sections.empty())
        output_sections.push_back(osec);
      osec->sections.push_back(isec);
    }
  }
  output_section_timer.stopTimer();

  file_offset_timer.startTimer();
  for_each(output_sections, [](OutputSection *osec) {
                              uint64_t off = 0;
                              for (InputSection *isec : osec->sections) {
                                off = align_to(off, isec->get_alignment());
                                isec->output_file_offset = off;
                                off += isec->get_size();
                              }
                            });
  file_offset_timer.stopTimer();

#if 0
  int max_align = 0;
  for (OutputSection *osec : output_sections) {
    for (InputSection *isec : osec->sections) {
      if (isec->get_alignment() > max_align) {
        llvm::outs() << toString(isec) << " " << isec->get_alignment() << "\n";
        max_align = isec->get_alignment();
      }       
    }
  }
#endif

#if 0
  int64_t filesize = 0;
  for (OutputSection *sec : output_sections) {
    sec->set_offset(filesize);
    filesize += sec->get_size();
  }
#endif

  out::ehdr = new OutputEhdr;
  out::shdr = new OutputShdr;
  out::phdr = new OutputPhdr;

  llvm::outs() << "         osec=" << output_sections.size() << "\n";
  llvm::outs() << "  num_defined=" << num_defined << "\n"
               << "num_undefined=" << num_undefined << "\n";

  llvm::TimerGroup::printAll(llvm::outs());
  llvm::outs().flush();
  _exit(0);
}

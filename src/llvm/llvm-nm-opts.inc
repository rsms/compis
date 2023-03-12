/*===- TableGen'erated file -------------------------------------*- C++ -*-===*\
|*                                                                            *|
|* Option Parsing Definitions                                                 *|
|*                                                                            *|
|* Automatically generated file, do not edit!                                 *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

/////////
// Prefixes

#ifdef PREFIX
#define COMMA ,
PREFIX(prefix_0, {nullptr})
PREFIX(prefix_3, {"-" COMMA nullptr})
PREFIX(prefix_1, {"--" COMMA nullptr})
PREFIX(prefix_2, {"--" COMMA "-" COMMA nullptr})
#undef COMMA
#endif // PREFIX

/////////
// Groups

#ifdef OPTION
OPTION(nullptr, "kind", grp_mach_o, Group, INVALID, INVALID, nullptr, 0, 0,
       "llvm-nm Mach-O Specific Options", nullptr, nullptr)
OPTION(nullptr, "kind", grp_xcoff_o, Group, INVALID, INVALID, nullptr, 0, 0,
       "llvm-nm XCOFF Specific Options", nullptr, nullptr)

//////////
// Options

OPTION(prefix_0, &"<input>"[0], INPUT, Input, INVALID, INVALID, nullptr, 0, 0, nullptr, nullptr, nullptr)
OPTION(prefix_0, &"<unknown>"[0], UNKNOWN, Unknown, INVALID, INVALID, nullptr, 0, 0, nullptr, nullptr, nullptr)
OPTION(prefix_1, &"--add-dyldinfo"[2], add_dyldinfo, Flag, grp_mach_o, INVALID, nullptr, 0, 0,
       "Add symbols from the dyldinfo not already in the symbol table", nullptr, nullptr)
OPTION(prefix_1, &"--add-inlinedinfo"[2], add_inlinedinfo, Flag, grp_mach_o, INVALID, nullptr, 0, 0,
       "Add symbols from the inlined libraries, TBD only", nullptr, nullptr)
OPTION(prefix_1, &"--arch="[2], arch_EQ, Joined, grp_mach_o, INVALID, nullptr, 0, 0,
       "architecture(s) from a Mach-O file to dump", nullptr, nullptr)
OPTION(prefix_2, &"--arch"[2], anonymous_2, Separate, INVALID, arch_EQ, nullptr, 0, 0, nullptr, nullptr, nullptr)
OPTION(prefix_3, &"-A"[1], anonymous_6, Flag, INVALID, print_file_name, nullptr, 0, 0,
       "Alias for --print-file-name", nullptr, nullptr)
OPTION(prefix_3, &"-a"[1], anonymous_5, Flag, INVALID, debug_syms, nullptr, 0, 0,
       "Alias for --debug-syms", nullptr, nullptr)
OPTION(prefix_3, &"-B"[1], anonymous_7, Flag, INVALID, format_EQ, "bsd\0", 0, 0,
       "Alias for --format=bsd", nullptr, nullptr)
OPTION(prefix_3, &"-C"[1], anonymous_8, Flag, INVALID, demangle, nullptr, 0, 0,
       "Alias for --demangle", nullptr, nullptr)
OPTION(prefix_1, &"--debug-syms"[2], debug_syms, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Show all symbols, even debugger only", nullptr, nullptr)
OPTION(prefix_1, &"--defined-only"[2], defined_only, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Show only defined symbols", nullptr, nullptr)
OPTION(prefix_1, &"--demangle"[2], demangle, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Demangle C++ symbol names", nullptr, nullptr)
OPTION(prefix_1, &"--dyldinfo-only"[2], dyldinfo_only, Flag, grp_mach_o, INVALID, nullptr, 0, 0,
       "Show only symbols from the dyldinfo", nullptr, nullptr)
OPTION(prefix_1, &"--dynamic"[2], dynamic, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Display dynamic symbols instead of normal symbols", nullptr, nullptr)
OPTION(prefix_3, &"-D"[1], anonymous_9, Flag, INVALID, dynamic, nullptr, 0, 0,
       "Alias for --dynamic", nullptr, nullptr)
OPTION(prefix_1, &"--export-symbols"[2], export_symbols, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Export symbol list for all inputs", nullptr, nullptr)
OPTION(prefix_1, &"--extern-only"[2], extern_only, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Show only external symbols", nullptr, nullptr)
OPTION(prefix_1, &"--format="[2], format_EQ, Joined, INVALID, INVALID, nullptr, 0, 0,
       "Specify output format: bsd (default), posix, sysv, darwin, just-symbols", "<format>", nullptr)
OPTION(prefix_1, &"--format"[2], anonymous_0, Separate, INVALID, format_EQ, nullptr, 0, 0, nullptr, "<format>", nullptr)
OPTION(prefix_3, &"-f"[1], anonymous_10, JoinedOrSeparate, INVALID, format_EQ, nullptr, 0, 0,
       "Alias for --format", "<format>", nullptr)
OPTION(prefix_3, &"-g"[1], anonymous_12, Flag, INVALID, extern_only, nullptr, 0, 0,
       "Alias for --extern-only", nullptr, nullptr)
OPTION(prefix_1, &"--help"[2], help, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Display this help", nullptr, nullptr)
OPTION(prefix_3, &"-h"[1], anonymous_11, Flag, INVALID, help, nullptr, 0, 0,
       "Alias for --help", nullptr, nullptr)
OPTION(prefix_1, &"--just-symbol-name"[2], anonymous_3, Flag, INVALID, format_EQ, "just-symbols\0", HelpHidden, 0,
       "Alias for --format=just-symbols", nullptr, nullptr)
OPTION(prefix_3, &"-j"[1], anonymous_13, Flag, INVALID, format_EQ, "just-symbols\0", 0, 0,
       "Alias for --format=just-symbols", nullptr, nullptr)
OPTION(prefix_3, &"-M"[1], anonymous_15, Flag, INVALID, print_armap, nullptr, HelpHidden, 0,
       "Deprecated alias for --print-armap", nullptr, nullptr)
OPTION(prefix_3, &"-m"[1], anonymous_14, Flag, INVALID, format_EQ, "darwin\0", 0, 0,
       "Alias for --format=darwin", nullptr, nullptr)
OPTION(prefix_1, &"--no-demangle"[2], no_demangle, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Don't demangle symbol names", nullptr, nullptr)
OPTION(prefix_1, &"--no-dyldinfo"[2], no_dyldinfo, Flag, grp_mach_o, INVALID, nullptr, 0, 0,
       "Don't add any symbols from the dyldinfo", nullptr, nullptr)
OPTION(prefix_1, &"--no-llvm-bc"[2], no_llvm_bc, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Disable LLVM bitcode reader", nullptr, nullptr)
OPTION(prefix_1, &"--no-rsrc"[2], no_rsrc, Flag, grp_xcoff_o, INVALID, nullptr, 0, 0,
       "Exclude resource file symbols (__rsrc) from the export symbol list.", nullptr, nullptr)
OPTION(prefix_1, &"--no-sort"[2], no_sort, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Show symbols in order encountered", nullptr, nullptr)
OPTION(prefix_1, &"--no-weak"[2], no_weak, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Show only non-weak symbols", nullptr, nullptr)
OPTION(prefix_1, &"--numeric-sort"[2], numeric_sort, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Sort symbols by address", nullptr, nullptr)
OPTION(prefix_3, &"-n"[1], anonymous_16, Flag, INVALID, numeric_sort, nullptr, 0, 0,
       "Alias for --numeric-sort", nullptr, nullptr)
OPTION(prefix_3, &"-o"[1], anonymous_17, Flag, INVALID, print_file_name, nullptr, 0, 0,
       "Alias for --print-file-name", nullptr, nullptr)
OPTION(prefix_1, &"--portability"[2], anonymous_4, Flag, INVALID, format_EQ, "posix\0", 0, 0,
       "Alias for --format=posix", nullptr, nullptr)
OPTION(prefix_1, &"--print-armap"[2], print_armap, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Print the archive map", nullptr, nullptr)
OPTION(prefix_1, &"--print-file-name"[2], print_file_name, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Precede each symbol with the object file it came from", nullptr, nullptr)
OPTION(prefix_1, &"--print-size"[2], print_size, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Show symbol size as well as address", nullptr, nullptr)
OPTION(prefix_3, &"-P"[1], anonymous_19, Flag, INVALID, format_EQ, "posix\0", 0, 0,
       "Alias for --format=posix", nullptr, nullptr)
OPTION(prefix_3, &"-p"[1], anonymous_18, Flag, INVALID, no_sort, nullptr, 0, 0,
       "Alias for --no-sort", nullptr, nullptr)
OPTION(prefix_1, &"--quiet"[2], quiet, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Suppress 'no symbols' diagnostic", nullptr, nullptr)
OPTION(prefix_1, &"--radix="[2], radix_EQ, Joined, INVALID, INVALID, nullptr, 0, 0,
       "Radix (o/d/x) for printing symbol Values", "<radix>", nullptr)
OPTION(prefix_1, &"--radix"[2], anonymous_1, Separate, INVALID, radix_EQ, nullptr, 0, 0, nullptr, "<radix>", nullptr)
OPTION(prefix_1, &"--reverse-sort"[2], reverse_sort, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Sort in reverse order", nullptr, nullptr)
OPTION(prefix_3, &"-r"[1], anonymous_20, Flag, INVALID, reverse_sort, nullptr, 0, 0,
       "Alias for --reverse-sort", nullptr, nullptr)
OPTION(prefix_1, &"--size-sort"[2], size_sort, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Sort symbols by size", nullptr, nullptr)
OPTION(prefix_1, &"--special-syms"[2], special_syms, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Do not filter special symbols from the output", nullptr, nullptr)
OPTION(prefix_3, &"-S"[1], anonymous_21, Flag, INVALID, print_size, nullptr, 0, 0,
       "Alias for --print-size", nullptr, nullptr)
OPTION(prefix_3, &"-s"[1], s, Flag, grp_mach_o, INVALID, nullptr, 0, 0,
       "Dump only symbols from this segment and section name", nullptr, nullptr)
OPTION(prefix_3, &"-t"[1], anonymous_22, JoinedOrSeparate, INVALID, radix_EQ, nullptr, 0, 0,
       "Alias for --radix", "<radix>", nullptr)
OPTION(prefix_1, &"--undefined-only"[2], undefined_only, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Show only undefined symbols", nullptr, nullptr)
OPTION(prefix_3, &"-U"[1], anonymous_24, Flag, INVALID, defined_only, nullptr, 0, 0,
       "Alias for --defined-only", nullptr, nullptr)
OPTION(prefix_3, &"-u"[1], anonymous_23, Flag, INVALID, undefined_only, nullptr, 0, 0,
       "Alias for --undefined-only", nullptr, nullptr)
OPTION(prefix_1, &"--version"[2], version, Flag, INVALID, INVALID, nullptr, 0, 0,
       "Display the version", nullptr, nullptr)
OPTION(prefix_3, &"-V"[1], anonymous_26, Flag, INVALID, version, nullptr, 0, 0,
       "Alias for --version", nullptr, nullptr)
OPTION(prefix_3, &"-v"[1], anonymous_25, Flag, INVALID, numeric_sort, nullptr, 0, 0,
       "Alias for --numeric-sort", nullptr, nullptr)
OPTION(prefix_1, &"--without-aliases"[2], without_aliases, Flag, INVALID, INVALID, nullptr, HelpHidden, 0,
       "Exclude aliases from output", nullptr, nullptr)
OPTION(prefix_3, &"-W"[1], anonymous_27, Flag, INVALID, no_weak, nullptr, 0, 0,
       "Alias for --no-weak", nullptr, nullptr)
OPTION(prefix_3, &"-X"[1], X, JoinedOrSeparate, INVALID, INVALID, nullptr, 0, 0,
       "Specifies the type of ELF, XCOFF, or IR object file to examine. The value must be one of: 32, 64, 32_64, any (default)", nullptr, nullptr)
OPTION(prefix_3, &"-x"[1], x, Flag, grp_mach_o, INVALID, nullptr, 0, 0,
       "Print symbol entry in hex", nullptr, nullptr)
#endif // OPTION

#ifdef SIMPLE_ENUM_VALUE_TABLE

struct SimpleEnumValue {
  const char *Name;
  unsigned Value;
};

struct SimpleEnumValueTable {
  const SimpleEnumValue *Table;
  unsigned Size;
};
static const SimpleEnumValueTable SimpleEnumValueTables[] = {};
static const unsigned SimpleEnumValueTablesSize = sizeof(SimpleEnumValueTables) / sizeof(SimpleEnumValueTable);
#endif // SIMPLE_ENUM_VALUE_TABLE


#ifdef OPTTABLE_ARG_INIT
//////////
// Option Values


#endif // OPTTABLE_ARG_INIT

#include "graph-properties-convert.h"
#include "galois/Galois.h"
#include "galois/Timer.h"
#include "galois/ErrorCode.h"
#include "galois/Logging.h"

#include <llvm/Support/CommandLine.h>

namespace cll = llvm::cl;

static cll::opt<std::string> input_filename(cll::Positional,
                                            cll::desc("<input file/directory>"),
                                            cll::Required);
static cll::opt<std::string>
    output_directory(cll::Positional,
                     cll::desc("<local ouput directory/s3 directory>"),
                     cll::Required);
static cll::opt<galois::SourceType>
    type(cll::desc("Input file type:"),
         cll::values(clEnumValN(galois::SourceType::kGraphml, "graphml",
                                "source file is of type GraphML"),
                     clEnumValN(galois::SourceType::kJson, "json",
                                "source file is of type JSON"),
                     clEnumValN(galois::SourceType::kCsv, "csv",
                                "source file is of type CSV")),
         cll::Required);
static cll::opt<galois::SourceDatabase>
    database(cll::desc("Database the data was exported from:"),
             cll::values(clEnumValN(galois::SourceDatabase::kNeo4j, "neo4j",
                                    "source data came from Neo4j"),
                         clEnumValN(galois::SourceDatabase::kMongodb, "mongodb",
                                    "source data came from mongodb")),
             cll::init(galois::SourceDatabase::kNone));
static cll::opt<size_t> chunk_size(
    "chunkSize",
    cll::desc("Chunk size for in memory arrow representation during "
              "converions, generally this term can be ignored but for sparse "
              "datasets it can be decreased for a smaller memory footprint"),
    cll::init(25000));

void ParseWild() {
  switch (type) {
  case galois::SourceType::kGraphml: {
    auto graph = galois::ConvertGraphml(input_filename, chunk_size);
    galois::ConvertToPropertyGraphAndWrite(graph, output_directory);
    break;
  }
  default: {
    GALOIS_LOG_ERROR("Only graphml files are supported for wild datasets");
  }
  }
}

void ParseNeo4j() {
  galois::GraphComponents graph{nullptr, nullptr, nullptr, nullptr, nullptr};
  switch (type) {
  case galois::SourceType::kGraphml:
    graph = galois::ConvertGraphml(input_filename, chunk_size);
    break;
  case galois::SourceType::kJson:
    graph = galois::ConvertNeo4jJson(input_filename);
    break;
  case galois::SourceType::kCsv:
    graph = galois::ConvertNeo4jCsv(input_filename);
    break;
  }
  galois::ConvertToPropertyGraphAndWrite(graph, output_directory);
}

void ParseMongodb() {
  switch (type) {
  case galois::SourceType::kJson:
    GALOIS_LOG_WARN("MongoDB importing is under development");
    break;
  default:
    GALOIS_LOG_ERROR("Only json files are supported for MongoDB exports");
  }
}

int main(int argc, char** argv) {
  galois::SharedMemSys sys;
  llvm::cl::ParseCommandLineOptions(argc, argv);

  galois::StatTimer total_timer("TimerTotal");
  total_timer.start();
  if (chunk_size == 0) {
    chunk_size = 25000;
  }

  switch (database) {
  case galois::SourceDatabase::kNone:
    ParseWild();
    break;
  case galois::SourceDatabase::kNeo4j:
    ParseNeo4j();
    break;
  case galois::SourceDatabase::kMongodb:
    ParseMongodb();
    break;
  }

  total_timer.stop();
}

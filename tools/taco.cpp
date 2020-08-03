#include <cstdlib>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <memory>

#include "taco.h"

#include "taco/error.h"
#include "taco/parser/parser.h"
#include "taco/storage/storage.h"
#include "taco/ir/ir.h"
#include "taco/ir/ir_printer.h"
#include "taco/index_notation/kernel.h"
#include "lower/iteration_graph.h"
#include "taco/lower/lower.h"
#include "taco/codegen/module.h"
#include "codegen/codegen_c.h"
#include "codegen/codegen_cuda.h"
#include "codegen/codegen.h"
#include "taco/util/strings.h"
#include "taco/util/files.h"
#include "taco/util/timers.h"
#include "taco/util/fill.h"
#include "taco/util/env.h"
#include "taco/util/collections.h"
#include "taco/cuda.h"
#include "taco/index_notation/transformations.h"
#include "taco/index_notation/index_notation_visitor.h"
#include "taco/index_notation/index_notation_nodes.h"

using namespace std;
using namespace taco;

#define TOOL_BENCHMARK_REPEAT(CODE, NAME, REPEAT) {              \
    if (time) {                                                  \
      TACO_TIME_REPEAT(CODE,REPEAT,timevalue,false);             \
      cout << NAME << " time (ms)" << endl << timevalue << endl; \
    }                                                            \
    else {                                                       \
      CODE;                                                      \
    }                                                            \
}

#define TOOL_BENCHMARK_TIMER(CODE,NAME,TIMER) {                  \
    if (time) {                                                  \
      taco::util::Timer timer;                                   \
      timer.start();                                             \
      CODE;                                                      \
      timer.stop();                                              \
      taco::util::TimeResults result = timer.getResult();        \
      cout << NAME << " " << result << " ms" << endl;            \
      TIMER=result;                                              \
    }                                                            \
    else {                                                       \
      CODE;                                                      \
    }                                                            \
}

static void printFlag(string flag, string text) {
  const size_t descriptionStart = 30;
  const size_t columnEnd        = 80;
  string flagString = "  -" + flag +
                      util::repeat(" ",descriptionStart-(flag.size()+3));
  cout << flagString;
  size_t column = flagString.size();
  vector<string> words = util::split(text, " ");
  for (auto& word : words) {
    if (column + word.size()+1 >= columnEnd) {
      cout << endl << util::repeat(" ", descriptionStart);
      column = descriptionStart;
    }
    column += word.size()+1;
    cout << word << " ";
  }
  cout << endl;
}

static const string fileFormats = "(.tns .ttx .mtx .rb)";

static void printUsageInfo() {
  cout << "Usage: taco <index expression> [options]" << endl;
  cout << endl;
  cout << "Examples:" << endl;
  cout << "  taco \"a(i) = b(i) + c(i)\"                            # Dense vector add" << endl;
  cout << "  taco \"a(i) = b(i) + c(i)\" -f=b:s -f=c:s -f=a:s       # Sparse vector add" << endl;
  cout << "  taco \"a(i) = B(i,j) * c(j)\" -f=B:ds                  # SpMV" << endl;
  cout << "  taco \"A(i,l) = B(i,j,k) * C(j,l) * D(k,l)\" -f=B:sss  # MTTKRP" << endl;
  cout << endl;
  cout << "Options:" << endl;
  printFlag("d=<var/tensor>:<size>",
            "Specify the dimension of tensor modes. This can be done by either "
            "specifying the dimension of index variables, or by specifying the "
            "dimension of tensor modes. All dimensions default to 42. "
            "Examples: i:5, j:100, b:5, A:10,10.");
  cout << endl;
  printFlag("f=<tensor>:<format>",
            "Specify the format of a tensor in the expression. Formats are "
            "specified per dimension using d (dense), s (sparse), "
            "u (sparse, not unique), q (singleton), or c (singleton, not unique). "
            "All formats default to dense. "
            "The ordering of modes can also be optionally specified as a "
            "comma-delimited list of modes in the order they should be stored. "
            "Examples: A:ds (i.e., CSR), B:ds:1,0 (i.e., CSC), c:d (i.e., "
            "dense vector), D:sss (i.e., CSF).");
  cout << endl;
  printFlag("t=<tensor>:<data type>",
            "Specify the data type of a tensor (defaults to double)."
            "Currently loaded tensors must be double."
            "Available types: uint8, uint16, uint32, uint64, uchar, ushort, uint, ulong, ulonglong,"
            "int8, int16, int32, int64, char, short, int, long, longlong,"
            "float, double, complexfloat, complexdouble"
            "Examples: A:uint16, b:long and D:complexfloat.");
  cout << endl;
  printFlag("c",
            "Generate compute kernel that simultaneously does assembly.");
  cout << endl;
  printFlag("i=<tensor>:<filename>",
            "Read a tensor from a file " + fileFormats + ".");
  cout << endl;
  printFlag("o=<tensor>:<filename>",
            "Write a tensor to a file " + fileFormats + ".");
  cout << endl;
  printFlag("O=<directory path>",
            "Write all tensors to a directory in the .tns format "
            "(defaults to $TMPDIR)");
  cout << endl;
  printFlag("g=<tensor>:<fill>",
            "Generate data for a vector or matrix. Vectors can be "
            "d (dense sequence), r (dense random), s (sparse) or h "
            "(hypersparse). Matrices can be d, s, h or l (slicing), f (FEM), "
            "b (Blocked). Examples: B:s, c:r.");
  cout << endl;
  printFlag("time=<repeat>",
            "Time compilation, assembly and <repeat> times computation "
            "(defaults to 1).");
  cout << endl;
  printFlag("write-time=<filename>",
            "Write computation times in csv format to <filename> "
            "as compileTime,assembleTime,mean,stdev,median.");
  cout << endl;
  printFlag("write-compute=<filename>",
            "Write the compute kernel to a file.");
  cout << endl;
  printFlag("write-assembly=<filename>",
            "Write the assembly kernel to a file.");
  cout << endl;
  printFlag("write-source=<filename>",
            "Write the C source code of the kernel functions of the given "
            "expression to a file.");
  cout << endl;
  printFlag("read-source=<filename>",
            "Read C kernels from the file. The argument order is inferred from "
            "the index expression. If the -time option is used then the given "
            "expression and kernels are timed.");
  cout << endl;
  printFlag("verify",
            "Compare results of generated and read kernels");
  cout << endl;
  printFlag("print-compute",
            "Print the compute kernel (default).");
  cout << endl;
  printFlag("print-assembly",
            "Print the assembly kernel.");
  cout << endl;
  printFlag("print-evaluate",
            "Print the evaluate kernel.");
  cout << endl;
  printFlag("print-kernels",
            "Print all kernels as a C library.");
  cout << endl;
  printFlag("print-concrete",
            "Print the concrete index notation of this expression.");
  cout << endl;
  printFlag("print-iteration-graph",
            "Print the iteration graph of this expression in the dot format.");
  cout << endl;
  printFlag("print-nocolor", "Print without colors.");
  cout << endl;
  printFlag("cuda", "Generate CUDA code for NVIDIA GPUs");
  cout << endl;
  printFlag("schedule", "Specify parallel execution schedule");
  cout << endl;
  printFlag("nthreads", "Specify number of threads for parallel execution");
}

static int reportError(string errorMessage, int errorCode) {
  cerr << "Error: " << errorMessage << endl << endl;
  printUsageInfo();
  return errorCode;
}

static void printCommandLine(ostream& os, int argc, char* argv[]) {
  taco_iassert(argc > 0);
  os << argv[0];
  if (argc > 1) {
    os << " \"" << argv[1] << "\"";
  }
  for (int i = 2; i < argc; i++) {
    os << " " << argv[i];
  }
}

static void setSchedulingCommands(istream& in, ostream& out, parser::Parser& parser, IndexStmt& stmt) {  
  auto findVar = [&stmt](string name) {
    ProvenanceGraph graph(stmt); 
    for (auto v : graph.getAllIndexVars()) {
      if (v.getName() == name) {
        return v;
      }
    }

    taco_uerror << "Index variable not defined in statement.";
  };

  auto getInput = [&in, &out](string prompt, auto &var) {
    out << prompt; 
    in >> var; 
  };

  out << endl << "To exit, input 'q'." << endl << endl;
  while (true) {
    string command;
    getInput("Enter a command: ", command); 

    if (command == "pos") {
      string i, ipos; 
      getInput("Enter the index variable to transform: ", i);
      getInput("Enter the derived position index variable: ", ipos);

      string tensor;
      getInput("Enter the tensor to perform the position cut on: ", tensor);

      for (auto a : getArgumentAccesses(stmt)) {
        if (a.getTensorVar().getName() == tensor) {
          IndexVar derived(ipos);
          stmt = stmt.pos(findVar(i), derived, a);
          goto end;
        }
      }
      out << "Tensor access not defined in statement." << endl;

    } else if (command == "fuse") {
      string i, j, f; 
      getInput("Enter the outer index variable to fuse: ", i);
      getInput("Enter the inner index variable to fuse: ", j);
      getInput("Enter the fused index variable: ", f);  

      IndexVar fused(f); 
      stmt = stmt.fuse(findVar(i), findVar(j), fused); 

    } else if (command == "split") {
      string i, i1, i2; 
      getInput("Enter the index variable to split: ", i);
      getInput("Enter the split outer index variable: ", i1);
      getInput("Enter the split inner index variable: ", i2);

      size_t splitFactor; 
      getInput("Enter the split factor: ", splitFactor);

      IndexVar split1(i1);
      IndexVar split2(i2);
      stmt = stmt.split(findVar(i), split1, split2, splitFactor);

    } else if (command == "divide") {
      string i, i1, i2; 
      getInput("Enter the index variable to divide: ", i);
      getInput("Enter the divided outer index variable: ", i1);
      getInput("Enter the divided inner index variable: ", i2);

      size_t divideFactor; 
      getInput("Enter the divide factor: ", divideFactor);

      IndexVar divide1(i1);
      IndexVar divide2(i2);
      stmt = stmt.divide(findVar(i), divide1, divide2, divideFactor);

    } else if (command == "precompute") {
      string i, iw, exprStr; 
      getInput("Enter the index variable to precompute over: ", i);
      getInput("Enter the index variable to precompute with: ", iw); 
      getInput("Enter (without spaces) the expression to precompute: ", exprStr); 

      IndexVar orig = findVar(i);
      IndexVar pre; 
      try {
        pre = findVar(iw);
      } catch (exception& e) {
        pre = IndexVar(iw);
      }

      struct GetExpr : public IndexNotationVisitor {
        using IndexNotationVisitor::visit;
        
        string exprStr; 
        IndexExpr expr; 

        void setExprStr(string input) {
          exprStr = input; 
          exprStr.erase(remove(exprStr.begin(), exprStr.end(), ' '), exprStr.end()); 
        }

        string toString(IndexExpr e) {
          stringstream tempStream; 
          tempStream << e; 
          string tempStr = tempStream.str();
          tempStr.erase(remove(tempStr.begin(), tempStr.end(), ' '), tempStr.end());
          return tempStr;
        }
        
        void visit(const AccessNode* node) {
          IndexExpr currentExpr(node); 
          if (toString(currentExpr) == exprStr) {
            expr = currentExpr; 
          }
          else {
            IndexNotationVisitor::visit(node);
          }
        }

        void visit(const UnaryExprNode* node) {
          IndexExpr currentExpr(node); 
          if (toString(currentExpr) == exprStr) {
            expr = currentExpr; 
          }
          else {
            IndexNotationVisitor::visit(node);
          }
        }

        void visit(const BinaryExprNode* node) {
          IndexExpr currentExpr(node);
          if (toString(currentExpr) == exprStr) {
            expr = currentExpr; 
          }
          else {
            IndexNotationVisitor::visit(node);
          }
        }
      };

      GetExpr visitor;
      visitor.setExprStr(exprStr); 
      stmt.accept(&visitor);

      Dimension dim = stmt.getIndexVarDomains().at(orig); 
      TensorVar workspace("workspace", Type(Float64, {dim}), Dense);
      stmt = stmt.precompute(visitor.expr, orig, pre, workspace);

    } else if (command == "reorder") {
      int n; 
      getInput("Enter the number of index variables to reorder: ", n);

      vector<IndexVar> reorderedVars; 
      for (int i = 0; i < n; i++) {
        string var;
        getInput("Enter an index variable to reorder: ", var);
        reorderedVars.push_back(findVar(var));
      }

      stmt = stmt.reorder(reorderedVars);

    } else if (command == "bound") {
      string i, i1; 
      getInput("Enter the index variable to bound: ", i);
      getInput("Enter the bounded index variable: ", i1);

      size_t bound;
      getInput("Enter the value to bound by: ", bound);

      string type; 
      getInput("Enter the bound type: ", type);

      BoundType bound_type; 
      if (type == "MinExact") { 
        bound_type = BoundType::MinExact; 
      } else if (type == "MinConstraint") { 
        bound_type = BoundType::MinConstraint; 
      } else if (type == "MaxExact") {
        bound_type = BoundType::MaxExact; 
      } else if (type == "MaxConstraint") {
        bound_type = BoundType::MaxConstraint; 
      } else {
        out << "Bound type not defined." << endl;
        goto end; 
      }

      IndexVar bound1(i1);
      stmt = stmt.bound(findVar(i), bound1, bound, bound_type);

    } else if (command == "unroll") {
      string i; 
      getInput("Enter the index variable to unroll: ", i);

      size_t unrollFactor; 
      getInput("Enter the unroll factor: ", unrollFactor);

      stmt = stmt.unroll(findVar(i), unrollFactor);
      
    } else if (command == "parallelize") {
      string i, unit, strategy; 
      getInput("Enter the index variable to parallelize: ", i);
      getInput("Enter the type of parallel hardware: ", unit);
      getInput("Enter the race strategy: ", strategy);

      ParallelUnit parallel_unit; 
      if (unit == "NotParallel") { 
        parallel_unit = ParallelUnit::NotParallel; 
      } else if (unit == "DefaultUnit") { 
        parallel_unit = ParallelUnit::DefaultUnit; 
      } else if (unit == "GPUBlock") {
        parallel_unit = ParallelUnit::GPUBlock;
      } else if (unit == "GPUWarp") {
        parallel_unit = ParallelUnit::GPUWarp;
      } else if (unit == "GPUThread") {
        parallel_unit = ParallelUnit::GPUThread;
      } else if (unit == "CPUThread") {
        parallel_unit = ParallelUnit::CPUThread; 
      } else if (unit == "CPUVector") {
        parallel_unit = ParallelUnit::CPUVector;
      } else if (unit == "CPUThreadGroupReduction") {
        parallel_unit = ParallelUnit::CPUThreadGroupReduction;
      } else if (unit == "CPUThreadGroupReduction") {
        parallel_unit = ParallelUnit::CPUThreadGroupReduction; 
      } else if (unit == "GPUWarpReduction") {
        parallel_unit = ParallelUnit::GPUWarpReduction; 
      } else {
        out << "Parallel hardware not defined." << endl;
        goto end; 
      }

      OutputRaceStrategy output_race_strategy; 
      if (strategy == "IgnoreRaces") {
        output_race_strategy = OutputRaceStrategy::IgnoreRaces; 
      } else if (strategy == "NoRaces") {
        output_race_strategy = OutputRaceStrategy::NoRaces; 
      } else if (strategy == "Atomics") { 
        output_race_strategy = OutputRaceStrategy::Atomics; 
      } else if (strategy == "Temporary") {
        output_race_strategy = OutputRaceStrategy::Temporary;
      } else if (strategy == "ParallelReduction") {
        output_race_strategy = OutputRaceStrategy::ParallelReduction;
      } else { 
        out << "Race strategy not defined." << endl;
        goto end; 
      }

      stmt = stmt.parallelize(findVar(i), parallel_unit, output_race_strategy);

    } else if (command == "q") {
      break; 
    } else {
      out << "Not a valid command";
      break; 
    }

    end: out << endl; 
  }

  out << endl << endl;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    printUsageInfo();
    return 0;
  }

  bool computeWithAssemble = false;

  bool printCompute        = false;
  bool printAssemble       = false;
  bool printEvaluate       = false;
  bool printKernels        = false;
  bool printConcrete       = false;
  bool printIterationGraph = false;

  bool writeCompute        = false;
  bool writeAssemble       = false;
  bool writeKernels        = false;
  bool loaded              = false;
  bool verify              = false;
  bool time                = false;
  bool writeTime           = false;

  bool color               = true;
  bool readKernels         = false;
  bool cuda                = false;

  bool setScheduleInteractive = false; 
  bool setScheduleManual      = false; 

  ParallelSchedule sched = ParallelSchedule::Static;
  int chunkSize = 0;
  int nthreads = 0;

  taco::util::TimeResults compileTime;
  taco::util::TimeResults assembleTime;
  
  int  repeat = 1;
  taco::util::TimeResults timevalue;

  string indexVarName = "";

  string exprStr;
  map<string,Format> formats;
  map<string,std::vector<int>> tensorsDimensions;
  map<string,Datatype> dataTypes;
  map<string,taco::util::FillMethod> tensorsFill;
  map<string,string> inputFilenames;
  map<string,string> outputFilenames;
  string outputDirectory;
  string writeComputeFilename;
  string writeAssembleFilename;
  string writeKernelFilename;
  string writeTimeFilename;
  vector<string> declaredTensors;

  vector<string> kernelFilenames;

  vector<string> scheduleCommands; 

  for (int i = 1; i < argc; i++) {
    string arg = argv[i];
    vector<string> argparts = util::split(arg, "=");
    if (argparts.size() > 2) {
      return reportError("Too many '\"' signs in argument", 5);
    }
    string argName = argparts[0];
    string argValue;
    if (argparts.size() == 2)
      argValue = argparts[1];

    if ("-f" == argName) {
      vector<string> descriptor = util::split(argValue, ":");
      if (descriptor.size() < 2 || descriptor.size() > 4) {
        return reportError("Incorrect format descriptor", 4);
      }
      string tensorName = descriptor[0];
      string formatString = descriptor[1];
      std::vector<ModeFormat> modeTypes;
      std::vector<ModeFormatPack> modeTypePacks;
      std::vector<int> modeOrdering;
      for (int i = 0; i < (int)formatString.size(); i++) {
        switch (formatString[i]) {
          case 'd':
            modeTypes.push_back(ModeFormat::Dense);
            break;
          case 's':
            modeTypes.push_back(ModeFormat::Sparse);
            break;
          case 'u':
            modeTypes.push_back(ModeFormat::Sparse(ModeFormat::NOT_UNIQUE));
            break;
          case 'c':
            modeTypes.push_back(ModeFormat::Singleton(ModeFormat::NOT_UNIQUE));
            break;
          case 'q':
            modeTypes.push_back(ModeFormat::Singleton);
            break;
          default:
            return reportError("Incorrect format descriptor", 3);
            break;
        }
        modeOrdering.push_back(i);
      }
      if (descriptor.size() > 2) {
        std::vector<std::string> modes = util::split(descriptor[2], ",");
        modeOrdering.clear();
        for (const auto& mode : modes) {
          modeOrdering.push_back(std::stoi(mode));
        }
      }
      if (descriptor.size() > 3) {
        std::vector<std::string> packBoundStrs = util::split(descriptor[3], ",");
        std::vector<int> packBounds(packBoundStrs.size());
        for (int i = 0; i < (int)packBounds.size(); ++i) {
          packBounds[i] = std::stoi(packBoundStrs[i]);
        }
        int pack = 0;
        std::vector<ModeFormat> modeTypesInPack;
        for (int i = 0; i < (int)modeTypes.size(); ++i) {
          if (i == packBounds[pack]) {
            modeTypePacks.push_back(modeTypesInPack);
            modeTypesInPack.clear();
            ++pack;
          }
          modeTypesInPack.push_back(modeTypes[i]);
        }
        modeTypePacks.push_back(modeTypesInPack);
      } else {
        for (const auto& modeType : modeTypes) {
          modeTypePacks.push_back(modeType);
        }
      }
      formats.insert({tensorName, Format(modeTypePacks, modeOrdering)});
    }
    else if ("-t" == argName) {
      vector<string> descriptor = util::split(argValue, ":");
      if (descriptor.size() != 2) {
        return reportError("Incorrect format descriptor", 3);
      }
      string tensorName = descriptor[0];
      string typesString = descriptor[1];
      Datatype dataType;
      if (typesString == "uint8") dataType = UInt8;
      else if(typesString == "uint16") dataType = UInt16;
      else if(typesString == "uint32") dataType = UInt32;
      else if(typesString == "uint64") dataType = UInt64;
      else if(typesString == "uchar") dataType = type<unsigned char>();
      else if(typesString == "ushort") dataType = type<unsigned short>();
      else if(typesString == "uint") dataType = type<unsigned int>();
      else if(typesString == "ulong") dataType = type<unsigned long>();
      else if(typesString == "ulonglong") dataType = type<unsigned long long>();
      else if(typesString == "int8") dataType = Int8;
      else if(typesString == "int16") dataType = Int16;
      else if(typesString == "int32") dataType = Int32;
      else if(typesString == "int64") dataType = Int64;
      else if(typesString == "char") dataType = type<char>();
      else if(typesString == "short") dataType = type<short>();
      else if(typesString == "int") dataType = type<int>();
      else if(typesString == "long") dataType = type<long>();
      else if(typesString == "longlong") dataType = type<long long>();
      else if(typesString == "float") dataType = Float32;
      else if(typesString == "double") dataType = Float64;
      else if(typesString == "complexfloat") dataType = Complex64;
      else if(typesString == "complexdouble") dataType = Complex128;
      else return reportError("Incorrect format descriptor", 3);
      dataTypes.insert({tensorName, dataType});
    }
    else if ("-d" == argName) {
      vector<string> descriptor = util::split(argValue, ":");
      string tensorName = descriptor[0];
      vector<string> dimensions = util::split(descriptor[1], ",");
      vector<int> tensorDimensions;
      for (size_t j=0; j<dimensions.size(); j++ ) {
        tensorDimensions.push_back(std::stoi(dimensions[j]));
      }
      tensorsDimensions.insert({tensorName, tensorDimensions});

    }
    else if ("-c" == argName) {
      computeWithAssemble = true;
    }
    else if ("-g" == argName) {
      vector<string> descriptor = util::split(argValue, ":");
      if (descriptor.size() < 2 || descriptor.size() > 3) {
        return reportError("Incorrect generating descriptor", 3);
      }
      string tensorName = descriptor[0];
      std::vector<taco::util::FillMethod> fillMethods;
      string fillString = descriptor[1];
      switch (fillString[0]) {
        case 'd': {
          tensorsFill.insert({tensorName, taco::util::FillMethod::Dense});
          break;
        }
        case 'u': {
          tensorsFill.insert({tensorName, taco::util::FillMethod::Uniform});
          break;
        }
        case 'r': {
          tensorsFill.insert({tensorName, taco::util::FillMethod::Random});
          break;
        }
        case 's': {
          tensorsFill.insert({tensorName, taco::util::FillMethod::Sparse});
          break;
        }
        case 'h': {
          tensorsFill.insert({tensorName, taco::util::FillMethod::HyperSparse});
          break;
        }
        case 'v': {
          tensorsFill.insert({tensorName, taco::util::FillMethod::SlicingV});
          break;
        }
        case 'l': {
          tensorsFill.insert({tensorName, taco::util::FillMethod::SlicingH});
          break;
        }
        case 'f': {
          tensorsFill.insert({tensorName, taco::util::FillMethod::FEM});
          break;
        }
        case 'b': {
          tensorsFill.insert({tensorName, taco::util::FillMethod::Blocked});
          break;
        }
        default: {
          return reportError("Incorrect generating descriptor", 3);
          break;
        }
      }
      loaded = true;
    }
    else if ("-i" == argName) {
      vector<string> descriptor = util::split(argValue, ":");
      if (descriptor.size() != 2) {
        return reportError("Incorrect -i usage", 3);
      }
      string tensorName = descriptor[0];
      string fileName  = descriptor[1];
      inputFilenames.insert({tensorName,fileName});
      loaded = true;
    }
    else if ("-o" == argName) {
      vector<string> descriptor = util::split(argValue, ":");
      if (descriptor.size() != 2) {
        return reportError("Incorrect -o usage", 3);
      }
      string tensorName = descriptor[0];
      string fileName  = descriptor[1];
      outputFilenames.insert({tensorName,fileName});
    }
    else if ("-O" == argName) {
      if (util::split(argValue, ":").size() > 1) {
        return reportError("Incorrect -O usage", 3);
      }
      outputDirectory = (argValue != "") ? argValue : util::getTmpdir();
    }
    else if ("-print-compute" == argName) {
      printCompute = true;
    }
    else if ("-print-assembly" == argName) {
      printAssemble = true;
    }
    else if ("-print-evaluate" == argName) {
      printEvaluate = true;
    }
    else if ("-print-concrete" == argName) {
      printConcrete = true;
    }
    else if ("-print-iteration-graph" == argName) {
      printIterationGraph = true;
    }
    else if ("-print-nocolor" == argName) {
      color = false;
    }
    else if ("-time" == argName) {
      time = true;
      if (argValue != "") {
        try {
          repeat=stoi(argValue);
        }
        catch (...) {
          return reportError("Incorrect time descriptor", 3);
        }
      }
    }
    else if ("-write-time" == argName) {
      writeTimeFilename = argValue;
      writeTime = true;
    }
    else if ("-verify" == argName) {
      verify = true;
    }
    else if ("-write-compute" == argName) {
      writeComputeFilename = argValue;
      writeCompute = true;
    }
    else if ("-write-assembly" == argName) {
      writeAssembleFilename = argValue;
      writeAssemble = true;
    }
    else if ("-write-source" == argName) {
      writeKernelFilename = argValue;
      writeKernels = true;
    }
    else if ("-read-source" == argName) {
      kernelFilenames.push_back(argValue);
      readKernels = true;
    }
    else if ("-cuda" == argName) {
      cuda = true;
    }
    else if ("-schedule" == argName) {
      vector<string> descriptor = util::split(argValue, ",");
      if (descriptor.size() > 2 || descriptor.empty()) {
        return reportError("Incorrect -schedule usage", 3);
      }
      if (descriptor[0] == "static") {
        sched = ParallelSchedule::Static;
      } else if (descriptor[0] == "dynamic") {
        sched = ParallelSchedule::Dynamic;
      } else {
        return reportError("Incorrect -schedule usage", 3);
      }
      if (descriptor.size() == 2) {
        try {
          chunkSize = stoi(descriptor[1]);
        }
        catch (...) {
          return reportError("Incorrect -schedule usage", 3);
        }
      }
    }
    else if ("-nthreads" == argName) {
      try {
        nthreads = stoi(argValue);
      }
      catch (...) {
        return reportError("Incorrect -nthreads usage", 3);
      }
    }
    else if ("-print-kernels" == argName) {
      printKernels = true;
    }
    else if ("-s" == argName) {
      if (argValue.empty()) {
        setScheduleInteractive = true; 
      } else {
        setScheduleManual = true; 
        std::replace_if(argValue.begin(), argValue.end(), [](char c) {
          if (c == '(' || c == ')' || c == ',') {
            return true; 
          }
          return false; 
        }, ' '); 
        scheduleCommands.push_back(argValue); 
      }
    }
    else {
      if (exprStr.size() != 0) {
        printUsageInfo();
        return 2;
      }
      exprStr = argv[i];
    }
  }

  // Print compute is the default if nothing else was asked for
  if (!printAssemble && !printEvaluate && !printIterationGraph &&
      !writeCompute && !writeAssemble && !writeKernels && !readKernels &&
      !printKernels && !loaded) {
    printCompute = true;
  }

  // Load tensors
  map<string,TensorBase> loadedTensors;

  // Load tensors
  for (auto& tensorNames : inputFilenames) {
    string name     = tensorNames.first;
    string filename = tensorNames.second;
    
    if (util::contains(dataTypes, name) && dataTypes.at(name) != Float64) {
      return reportError("Loaded tensors can only be type double", 7);
    }

    Format format = util::contains(formats, name) ? formats.at(name) : Dense;
    TensorBase tensor;
    TOOL_BENCHMARK_TIMER(tensor = read(filename,format,false),
                         name+" file read:", timevalue);
    tensor.setName(name);

    TOOL_BENCHMARK_TIMER(tensor.pack(), name+" pack:     ", timevalue);

    loadedTensors.insert({name, tensor});

    cout << tensor.getName()
         << " size: "
         << "(" << util::join(tensor.getDimensions(), " x ") << "), "
         << tensor.getStorage().getSizeInBytes() << " bytes" << endl;
  }

  if (exprStr == "") {
    return 0;
  }

  TensorBase tensor;
  parser::Parser parser(exprStr, formats, dataTypes, tensorsDimensions, loadedTensors, 42);
  try {
    parser.parse();
    tensor = parser.getResultTensor();
  } catch (parser::ParseError& e) {
    return reportError(e.getMessage(), 6);
  }

  // Generate tensors
  for (auto& fills : tensorsFill) {
    TensorBase tensor = parser.getTensor(fills.first);
    util::fillTensor(tensor,fills.second);

    loadedTensors.insert({fills.first, tensor});
    cout << tensor.getName()
         << " size: "
         << "(" << util::join(tensor.getDimensions(), " x ") << "), "
         << tensor.getStorage().getSizeInBytes() << " bytes" << endl;
  }

  // If all input tensors have been initialized then we should evaluate
  bool benchmark = true;
  for (auto& tensor : parser.getTensors()) {
    if (tensor.second == parser.getResultTensor()) {
      continue;
    }
    if (!util::contains(loadedTensors, tensor.second.getName())) {
      benchmark = false;
    }
  }

  if (cuda) {
    if (!CUDA_BUILT && benchmark) {
      return reportError("TACO must be built for CUDA (cmake -DCUDA=ON ..) to benchmark", 2);
    }
    set_CUDA_codegen_enabled(true);
  }
  else {
    set_CUDA_codegen_enabled(false);
  }

  ir::Stmt assemble;
  ir::Stmt compute;
  ir::Stmt evaluate;

  taco_set_parallel_schedule(sched, chunkSize);
  taco_set_num_threads(nthreads);

  IndexStmt stmt =
      makeConcreteNotation(makeReductionNotation(tensor.getAssignment()));
  stmt = reorderLoopsTopologically(stmt);
  stmt = insertTemporaries(stmt);
  stmt = parallelizeOuterLoop(stmt);

  if (setScheduleInteractive) {
    setSchedulingCommands(cin, cout, parser, stmt);
  } else if (setScheduleManual) {
    stringstream scheduleStream; 
    for (string command : scheduleCommands) {
      scheduleStream << command; 
    }

    stringstream throwaway;
    try {
      setSchedulingCommands(scheduleStream, throwaway, parser, stmt);
    } catch (TacoException e) {
      string msg = string(e.what());
      msg = msg.insert(msg.find(":\n") + 3, "Error with provided schedule: "); 
      throw TacoException(msg);
    }
  }

  if (printConcrete) {
    cout << stmt << endl;
  }

  Kernel kernel;
  if (benchmark) {
    if (time) cout << endl;

    shared_ptr<ir::Module> module(new ir::Module);

    TOOL_BENCHMARK_TIMER(
      compute = lower(stmt, "compute",  computeWithAssemble, true);
      assemble = lower(stmt, "assemble", true, false);
      evaluate = lower(stmt, "evaluate", true, true);

      module->addFunction(compute);
      module->addFunction(assemble);
      module->addFunction(evaluate);
      module->compile();
    , "Compile: ", compileTime);
      
    void* compute  = module->getFuncPtr("compute");
    void* assemble = module->getFuncPtr("assemble");
    void* evaluate = module->getFuncPtr("evaluate");
    kernel = Kernel(stmt, module, evaluate, assemble, compute);

    tensor.compileSource(util::toString(kernel));

    TOOL_BENCHMARK_TIMER(tensor.assemble(),"Assemble:",assembleTime);
    if (repeat == 1) {
      TOOL_BENCHMARK_TIMER(tensor.compute(), "Compute: ", timevalue);
    }
    else {
      TOOL_BENCHMARK_REPEAT(tensor.compute(), "Compute", repeat);
    }

    for (auto& kernelFilename : kernelFilenames) {
      TensorBase customTensor;

      std::fstream filestream;
      util::openStream(filestream, kernelFilename, ifstream::in);
      string kernelSource((std::istreambuf_iterator<char>(filestream)),
                          std::istreambuf_iterator<char>());
      filestream.close();

      // TODO: Replace this redundant parsing with just a call to set the expr
      try {
        auto operands = parser.getTensors();
        operands.erase(parser.getResultTensor().getName());
        parser::Parser parser2(exprStr, formats, dataTypes, tensorsDimensions,
                               operands, 42);
        parser2.parse();
        customTensor = parser2.getResultTensor();
      } catch (parser::ParseError& e) {
        return reportError(e.getMessage(), 6);
      }
      customTensor.compileSource(kernelSource);

      if (time) {
        cout << endl;
        cout << kernelFilename << ":" << endl;
      }
      TOOL_BENCHMARK_TIMER(customTensor.assemble(),"Assemble:", assembleTime);
      if (repeat == 1) {
        TOOL_BENCHMARK_TIMER(customTensor.compute(), "Compute: ", timevalue);
      }
      else {
        TOOL_BENCHMARK_REPEAT(customTensor.compute(), "Compute", repeat);
      }

      if (verify) {
        if (time) cout << endl;
        cout << "Verifying... ";
        bool eq = equals(customTensor, tensor);
        cout << "done" << endl;
        if (!eq) {
          cerr << "Error: " << "Results computed with " << kernelFilename <<
              " differ from those computed with the expression." <<
              "  Actual: " << customTensor << endl <<
              "Expected: " << tensor << endl;
          return 7;
        }
      }
    }
  }
  else {
    compute = lower(stmt, "compute",  computeWithAssemble, true);
    assemble = lower(stmt, "assemble", true, false);
    evaluate = lower(stmt, "evaluate", true, true);
  }

  string packComment = 
    "/*\n"
    " * The `pack` functions convert coordinate and value arrays in COO format,\n"
    " * with nonzeros sorted lexicographically by their coordinates, to the\n"
    " * specified input format.\n"
    " *\n"
    " * The `unpack` function converts the specified output format to coordinate\n"
    " * and value arrays in COO format.\n"
    " *\n"
    " * For both, the `_COO_pos` arrays contain two elements, where the first is 0\n"
    " * and the second is the number of nonzeros in the tensor.\n"
    " */"; 
  
  vector<ir::Stmt> packs; 
  for (auto a : getArgumentAccesses(stmt)) {
    TensorVar tensor = a.getTensorVar();
    if (tensor.getOrder() == 0) {
      continue;
    }

    std::string tensorName = tensor.getName();
    std::vector<IndexVar> indexVars = a.getIndexVars();

    IndexStmt packStmt = generatePackCOOStmt(tensor, indexVars, true);
    packs.push_back(lower(packStmt, "pack_" + tensorName, true, true, true));
  }

  ir::Stmt unpack;
  for (auto a : getResultAccesses(stmt).first) {
    TensorVar tensor = a.getTensorVar();
    if (tensor.getOrder() == 0) {
      continue;
    }

    std::vector<IndexVar> indexVars = a.getIndexVars();

    IndexStmt unpackStmt = generatePackCOOStmt(tensor, indexVars, false);
    unpack = lower(unpackStmt, "unpack", true, true, false, true); 
    break; // should only have one result access
  }

  string gentext = "// Generated by the Tensor Algebra Compiler (tensor-compiler.org)";
  if (printAssemble || printCompute) {
    std::string green = (color) ? "\033[38;5;70m" : "";
    std::string nc    = (color) ? "\033[0m"       : "";
    cout << green << gentext << nc << endl;
  }

  bool hasPrinted = false;
  std::shared_ptr<ir::CodeGen> codegen = ir::CodeGen::init_default(cout, ir::CodeGen::ImplementationGen);
  codegen->setColor(color);
  if (printAssemble) {
    if (assemble.defined()) {
      codegen->compile(assemble, false);
    }
    else {
      tensor.printAssembleIR(cout,color, true);
    }

    hasPrinted = true;
    std::cout << std::endl;
  }

  if (printCompute) {
    if (hasPrinted) {
      cout << endl;
    }

    if (compute.defined()) {
      codegen->compile(compute, false);
    }
    else {
      tensor.printComputeIR(cout, color, true);
    }

    hasPrinted = true;
    std::cout << std::endl;
  }

  if (printEvaluate && evaluate.defined()) {
    if (hasPrinted) {
      cout << endl;
    }
    codegen->compile(evaluate, false);
    hasPrinted = true;
    std::cout << std::endl;
  }

  if (printKernels) {
    if (hasPrinted) {
      cout << endl;
    }

    if (assemble.defined() ) {
      codegen->compile(assemble, false);
      cout << endl << endl;
    }

    if (compute.defined() ) {
      codegen->compile(compute, false);
      cout << endl << endl;
    }

    if (evaluate.defined() ) {
      codegen->compile(evaluate, false);
      cout << endl << endl;
    }

    if (unpack.defined()) {
      cout << endl << packComment << endl;
    }
    
    for (auto pack : packs) {
      codegen->compile(pack, false);
      cout << endl << endl;
    }

    if (unpack.defined() ) {
      codegen->compile(unpack, false);
      cout << endl << endl;
    }

    hasPrinted = true;
  }

  IterationGraph iterationGraph;
  if (printIterationGraph) {
    iterationGraph = IterationGraph::make(tensor.getAssignment());
  }

  if (printIterationGraph) {
    if (hasPrinted) {
      cout << endl << endl;
    }
    iterationGraph.printAsDot(cout);
    hasPrinted = true;
  }

  if (writeTime) {
    std::ofstream filestream;
    filestream.open(writeTimeFilename, std::ofstream::out|std::ofstream::trunc);
    filestream << compileTime << "," << assembleTime << "," << timevalue.mean
               << "," << timevalue.stdev << "," << timevalue.median << endl;
    filestream.close();
  }
  
  if (writeCompute) {
    std::ofstream filestream;
    filestream.open(writeComputeFilename,
                    std::ofstream::out|std::ofstream::trunc);
    filestream << gentext << endl << "// ";
    printCommandLine(filestream, argc, argv);
    filestream << endl;
    std::shared_ptr<ir::CodeGen> codegenFile = ir::CodeGen::init_default(filestream, ir::CodeGen::ImplementationGen);
    codegenFile->compile(compute, false);
    filestream.close();
  }

  if (writeAssemble) {
    std::ofstream filestream;
    filestream.open(writeAssembleFilename,
                    std::ofstream::out|std::ofstream::trunc);
    filestream << gentext << endl << "// ";
    printCommandLine(filestream, argc, argv);
    filestream << endl;
    std::shared_ptr<ir::CodeGen> codegenFile = ir::CodeGen::init_default(filestream, ir::CodeGen::ImplementationGen);
    codegenFile->compile(assemble, false);
    filestream.close();
  }

  if (writeKernels) {
    std::ofstream filestream;
    filestream.open(writeKernelFilename,
                    std::ofstream::out|std::ofstream::trunc);
    filestream << gentext << endl << "// ";
    printCommandLine(filestream, argc, argv);
    filestream << endl;
    std::shared_ptr<ir::CodeGen> codegenFile =
        ir::CodeGen::init_default(filestream, ir::CodeGen::ImplementationGen);
    bool hasPrinted = false;
    
    if (compute.defined() ) {
      codegenFile->compile(compute, !hasPrinted);
      hasPrinted = true;
    }
    if (assemble.defined() ) {
      codegenFile->compile(assemble, !hasPrinted);
      hasPrinted = true;
    }
    if (evaluate.defined() ) {
      codegenFile->compile(evaluate, !hasPrinted);
      hasPrinted = true;
    }

    if (unpack.defined() ) {
      filestream << endl << packComment << endl;
    }
    for (auto pack : packs) {
      codegenFile->compile(pack, !hasPrinted);
      hasPrinted = true;
    }
    if (unpack.defined() ) {
      codegenFile->compile(unpack, !hasPrinted);
      hasPrinted = true;
    }

    filestream.close();
  }

  for (auto& output : outputFilenames) {
    string tensorName = output.first;
    string filename = output.second;
    if (tensorName == tensor.getName()) {
      write(filename, tensor);
    }
    else if (util::contains(loadedTensors, tensorName)) {
      write(filename, loadedTensors.at(tensorName));
    }
    else {
      return reportError("Incorrect -o descriptor", 3);
    }
  }

  if (outputDirectory != "") {
    string outputFileName = outputDirectory + "/" + tensor.getName() + ".tns";
    write(outputFileName, FileType::tns, tensor);
    TensorBase paramTensor;
    for (const auto &fills : tensorsFill ) {
      paramTensor = parser.getTensor(fills.first);
      outputFileName = outputDirectory + "/" + paramTensor.getName() + ".tns";
      write(outputFileName, FileType::tns, paramTensor);
    }
  }

  return 0;
}

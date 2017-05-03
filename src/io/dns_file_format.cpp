#include "taco/io/dns_file_format.h"

#include <iostream>
#include <fstream>

#include "taco/tensor_base.h"
#include "taco/util/error.h"

using namespace std;

namespace taco {
namespace io {
namespace dns {

TensorBase read(std::string filename) {
  std::ifstream file;
  file.open(filename);
  taco_uassert(file.is_open()) << "Error opening file: " << filename;
  TensorBase tensor = read(file);
  file.close();
  return tensor;
}

TensorBase read(std::istream& stream) {
  taco_not_supported_yet;
  return TensorBase();
}

void write(std::string filename, const TensorBase& tensor) {
  std::ofstream file;
  file.open(filename);
  taco_uassert(file.is_open()) << "Error opening file: " << filename;
  write(file, tensor);
  file.close();
}

void write(std::ostream& stream, const TensorBase& tensor) {
}

}}}

#include <iostream>
#include "../../hook_functions/mcp_hooks.h"
int main (int argc, char *argv[]) {
  mcp_roi_begin();
  for (int i = 0; i < std::atoi(argv[1]); ++i) {
    printf("Loop 1\n");
  }
  for (int j = std::atoi(argv[1]); j <= std::atoi(argv[2]); j++) {
    printf("Loop 2\n");
  }
  return 0;
}
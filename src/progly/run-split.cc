/*
* Copyright (C) 2018 The Regents of the University of California
* All Rights Reserved
*
* This library can redistribute it and/or modify under the terms
* of the GNU Lesser General Public License Version 2.1 as published
* by the Free Software Foundation.
*
*/

// std libs
#include <fstream>
#include <boost/program_options.hpp>
#include <iostream>
#include <algorithm>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <chrono>
#include "re2/re2.h"

// ceph includes
#include "include/rados/librados.hpp"

// other includes
#include "cls/split/cls_split.h"

namespace po = boost::program_options;

int main(int argc, char **argv)
{
  std::cout << "starting splits..." << std::endl ;

  Split::split() ;

  std::cout << "...splits done. phew!" << std::endl ;
  return 0 ;
}

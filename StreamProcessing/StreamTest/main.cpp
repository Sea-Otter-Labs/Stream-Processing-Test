#include "StreamTest.h"
#include <iostream>
#include "Logger.h"

int main(int argc, char** argv)
{
    Logger::init(Logger::Level::Debug, "./logs", 10*1024*1024, 10);

    StreamTest test;
    test.start();
    return 0;
}
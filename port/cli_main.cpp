// The command-line and macOS app entry points share the same game runtime.
extern "C" int motorstorm_main(int argc, char** argv);

int main(int argc, char** argv)
{
    return motorstorm_main(argc, argv);
}

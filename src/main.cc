#include "shell.h"

#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

int main(int argc, char **argv) {
  try {
    std::string directory = "db";
    std::size_t frames = 32;

    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];

      if (option == "--help" || option == "-h") {
        std::cout
            << "Usage: vql [--db DIR] [--frames N]\n"
               "SQL and shell commands end with ;. Use HELP; for commands.\n";
        return 0;
      }

      if ((option != "--db" && option != "--frames") || i + 1 == argc)
        throw std::invalid_argument(
            "Expected --db DIR or --frames N; use --help");

      const std::string value = argv[++i];

      if (option == "--db") {
        directory = value;
      } else {
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), frames);

        if (error != std::errc{} || end != value.data() + value.size() ||
            frames < 4)
          throw std::invalid_argument(
              "frames must be an integer of at least 4");
      }
    }

    vql::Shell shell(directory, frames);
    return shell.run(std::cin, std::cout, std::cerr, isatty(STDIN_FILENO));
  } catch (const std::exception &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
}

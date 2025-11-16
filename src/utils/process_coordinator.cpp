/**
 * @file process_coordinator.cpp
 * @brief Multi-process coordinator for parallel WinAlign execution (Phase 5)
 *
 * Launches multiple winalign-gpu instances in parallel on different FASTQ chunks.
 * Monitors progress and aggregates results.
 */

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <future>
#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <sstream>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

struct ProcessTask {
    uint32_t chunk_id;
    std::string input_fastq;
    std::string output_bam;
    std::string reference;
    std::string winalign_binary;
    std::vector<std::string> extra_args;
    int return_code = -1;
    std::chrono::milliseconds duration{0};
};

/**
 * @brief Execute winalign process for a chunk
 */
int execute_winalign(ProcessTask& task) {
    auto start = std::chrono::high_resolution_clock::now();

    // Build command
    std::ostringstream cmd;
    cmd << task.winalign_binary
        << " -r " << task.reference
        << " -i " << task.input_fastq
        << " -o " << task.output_bam;

    for (const auto& arg : task.extra_args) {
        cmd << " " << arg;
    }

    std::string command = cmd.str();
    std::cout << "[Chunk " << task.chunk_id << "] Executing: " << command << std::endl;

    // Execute
#ifdef _WIN32
    task.return_code = _wsystem(command.c_str());
#else
    task.return_code = system(command.c_str());
#endif

    auto end = std::chrono::high_resolution_clock::now();
    task.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    if (task.return_code == 0) {
        std::cout << "[Chunk " << task.chunk_id << "] Completed successfully in "
                  << (task.duration.count() / 1000.0) << "s" << std::endl;
    } else {
        std::cerr << "[Chunk " << task.chunk_id << "] Failed with code "
                  << task.return_code << std::endl;
    }

    return task.return_code;
}

/**
 * @brief Monitor progress of all tasks
 */
void monitor_progress(const std::vector<std::future<int>>& futures,
                      const std::vector<ProcessTask>& tasks,
                      std::atomic<bool>& monitoring) {

    while (monitoring) {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        if (!monitoring) break;

        // Check status
        uint32_t completed = 0;
        for (size_t i = 0; i < futures.size(); i++) {
            if (futures[i].wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                completed++;
            }
        }

        std::cout << "[Monitor] Progress: " << completed << "/" << tasks.size()
                  << " chunks completed" << std::endl;
    }
}

/**
 * @brief Run winalign in parallel on multiple chunks
 */
int run_parallel(const std::vector<ProcessTask>& task_templates,
                 uint32_t max_parallel = 0) {

    // Auto-detect parallel limit
    if (max_parallel == 0) {
        max_parallel = std::thread::hardware_concurrency();
        if (max_parallel > 8) max_parallel = 8;  // Reasonable default
    }

    std::cout << "Running " << task_templates.size() << " tasks with max "
              << max_parallel << " parallel processes" << std::endl;

    std::vector<ProcessTask> tasks = task_templates;
    std::vector<std::future<int>> futures;
    std::atomic<bool> monitoring{true};

    // Launch monitor thread
    std::thread monitor_thread(monitor_progress, std::ref(futures), std::ref(tasks), std::ref(monitoring));

    auto start_time = std::chrono::high_resolution_clock::now();

    // Launch tasks in batches
    for (size_t i = 0; i < tasks.size(); i += max_parallel) {
        size_t batch_size = std::min(max_parallel, (uint32_t)(tasks.size() - i));

        std::cout << "\n=== Launching batch of " << batch_size << " tasks ===" << std::endl;

        // Launch batch
        std::vector<std::future<int>> batch_futures;
        for (size_t j = 0; j < batch_size; j++) {
            size_t task_idx = i + j;
            batch_futures.push_back(std::async(std::launch::async,
                                               execute_winalign,
                                               std::ref(tasks[task_idx])));
        }

        // Wait for batch to complete
        for (auto& future : batch_futures) {
            futures.push_back(std::move(future));
        }
    }

    // Wait for all tasks
    std::cout << "\n=== Waiting for all tasks to complete ===" << std::endl;
    int failed_count = 0;
    for (size_t i = 0; i < futures.size(); i++) {
        int result = futures[i].get();
        if (result != 0) {
            failed_count++;
        }
    }

    monitoring = false;
    if (monitor_thread.joinable()) {
        monitor_thread.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    // Summary
    std::cout << "\n=== Summary ===" << std::endl;
    std::cout << "Total tasks: " << tasks.size() << std::endl;
    std::cout << "Successful: " << (tasks.size() - failed_count) << std::endl;
    std::cout << "Failed: " << failed_count << std::endl;
    std::cout << "Total time: " << (total_duration.count() / 1000.0) << "s" << std::endl;

    // Per-task details
    std::cout << "\nPer-chunk details:" << std::endl;
    for (const auto& task : tasks) {
        std::cout << "  Chunk " << task.chunk_id << ": ";
        if (task.return_code == 0) {
            std::cout << "SUCCESS (" << (task.duration.count() / 1000.0) << "s)";
        } else {
            std::cout << "FAILED (code " << task.return_code << ")";
        }
        std::cout << std::endl;
    }

    return failed_count == 0 ? 0 : 1;
}

/**
 * @brief Print usage information
 */
void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options] -r <reference.fa> -i <chunk_pattern> -o <output_dir>\n"
              << "\n"
              << "Options:\n"
              << "  -r, --reference <file>    Reference genome FASTA\n"
              << "  -i, --input <pattern>     Input FASTQ chunk pattern (e.g., chunks/reads_chunk*.fastq)\n"
              << "  -o, --output <dir>        Output directory for BAM files\n"
              << "  -p, --parallel <N>        Max parallel processes (default: auto)\n"
              << "  -b, --binary <path>       Path to winalign-gpu binary (default: winalign-gpu)\n"
              << "  -h, --help                Show this help message\n"
              << "\n"
              << "Examples:\n"
              << "  " << prog_name << " -r ref.fa -i chunks/reads_chunk -o results -p 4\n"
              << "  " << prog_name << " -r ref.fa -i data/chunk*.fastq -o out --parallel 8\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    try {
        std::string reference;
        std::string input_pattern;
        std::string output_dir;
        std::string binary = "winalign-gpu";
        uint32_t max_parallel = 0;
        std::vector<std::string> extra_args;

        // Parse arguments
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];

            if ((arg == "-r" || arg == "--reference") && i + 1 < argc) {
                reference = argv[++i];
            } else if ((arg == "-i" || arg == "--input") && i + 1 < argc) {
                input_pattern = argv[++i];
            } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
                output_dir = argv[++i];
            } else if ((arg == "-p" || arg == "--parallel") && i + 1 < argc) {
                max_parallel = std::stoi(argv[++i]);
            } else if ((arg == "-b" || arg == "--binary") && i + 1 < argc) {
                binary = argv[++i];
            } else if (arg == "-h" || arg == "--help") {
                print_usage(argv[0]);
                return 0;
            } else {
                extra_args.push_back(arg);
            }
        }

        if (reference.empty() || input_pattern.empty() || output_dir.empty()) {
            print_usage(argv[0]);
            return 1;
        }

        // Create output directory
        fs::create_directories(output_dir);

        // Find all matching input files
        std::vector<std::string> input_files;
        fs::path pattern_path(input_pattern);
        fs::path parent = pattern_path.parent_path();
        std::string pattern_name = pattern_path.filename().string();

        if (parent.empty()) parent = ".";

        for (const auto& entry : fs::directory_iterator(parent)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                // Simple pattern matching (prefix-based)
                if (filename.find(pattern_name) == 0 ||
                    pattern_name.find('*') != std::string::npos) {
                    input_files.push_back(entry.path().string());
                }
            }
        }

        if (input_files.empty()) {
            std::cerr << "Error: No input files matching pattern: " << input_pattern << std::endl;
            return 1;
        }

        std::sort(input_files.begin(), input_files.end());

        std::cout << "Found " << input_files.size() << " input chunks" << std::endl;

        // Create tasks
        std::vector<ProcessTask> tasks;
        for (size_t i = 0; i < input_files.size(); i++) {
            ProcessTask task;
            task.chunk_id = i;
            task.input_fastq = input_files[i];

            // Generate output BAM path
            fs::path input_path(input_files[i]);
            std::string output_name = input_path.stem().string() + ".bam";
            task.output_bam = (fs::path(output_dir) / output_name).string();

            task.reference = reference;
            task.winalign_binary = binary;
            task.extra_args = extra_args;

            tasks.push_back(task);
        }

        // Run parallel execution
        return run_parallel(tasks, max_parallel);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

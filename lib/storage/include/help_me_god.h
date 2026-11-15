#ifndef HELP_ME_GOD_H
#define HELP_ME_GOD_H

#include "disk_manager.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace vql::storage::help_me_god {

std::uint16_t read_u16(const Page &page, std::size_t position);
void write_u16(Page &page, std::size_t position, std::uint16_t number);
std::uint32_t read_u32(const Page &page, std::size_t position);
void write_u32(Page &page, std::size_t position, std::uint32_t number);

std::size_t slot_position(std::size_t slot_id);
std::size_t slot_count(const Page &page);
std::size_t free_space(const Page &page);
bool is_live(const Page &page, std::size_t slot_id);
void initialize(Page &page, std::uint32_t page_id);
std::span<const char> get(const Page &page, std::size_t slot_id);
std::size_t insert(Page &page, std::span<const char> record);
void kill(Page &page, std::size_t slot_id);

void initialize_directory(Page &page, std::uint32_t page_id);
std::size_t directory_entry_count(const Page &page);
std::uint32_t next_directory(const Page &page);
void set_next_directory(Page &page, std::uint32_t page_id);
std::size_t directory_entry_position(std::size_t index);
std::size_t get_page_id(const Page &page, std::size_t index);
std::size_t get_page_free_bytes(const Page &page, std::size_t index);
void update_free_bytes(Page &page, std::size_t index, std::uint16_t free_bytes);
void append_entry(Page &page, std::uint32_t page_id, std::uint16_t free_bytes);

} // namespace vql::storage::help_me_god

#endif // HELP_ME_GOD_H

#include "help_me_god.h"

#include <algorithm>
#include <stdexcept>

namespace vql::storage::help_me_god {

std::uint16_t read_u16(const Page &page, std::size_t position) {
  auto low = static_cast<unsigned char>(page.at(position));
  auto high = static_cast<unsigned char>(page.at(position + 1));
  return static_cast<std::uint16_t>(low | (high << 8));
}

void write_u16(Page &page, std::size_t position, std::uint16_t number) {
  if (position >= page.size() || page.size() - position < 2)
    throw std::out_of_range("Not enough room for uint16!");

  page.at(position) = static_cast<char>(number & 0xFF);
  page.at(position + 1) = static_cast<char>((number >> 8) & 0xFF);
}

std::uint32_t read_u32(const Page &page, std::size_t position) {
  auto one = static_cast<unsigned char>(page.at(position));
  auto two = static_cast<unsigned char>(page.at(position + 1));
  auto tre = static_cast<unsigned char>(page.at(position + 2));
  auto fur = static_cast<unsigned char>(page.at(position + 3));
  return std::uint32_t{one} | (std::uint32_t{two} << 8) |
         (std::uint32_t{tre} << 16) | (std::uint32_t{fur} << 24);
}

void write_u32(Page &page, std::size_t position, std::uint32_t number) {
  if (position >= page.size() || page.size() - position < 4)
    throw std::out_of_range("Not enough room for uint32!");

  page.at(position) = static_cast<char>(number & 0xFF);
  page.at(position + 1) = static_cast<char>((number >> 8) & 0xFF);
  page.at(position + 2) = static_cast<char>((number >> 16) & 0xFF);
  page.at(position + 3) = static_cast<char>((number >> 24) & 0xFF);
}

std::size_t slot_position(std::size_t slot_id) { return 8 + 4 * slot_id; }

std::size_t slot_count(const Page &page) { return read_u16(page, 4); }

std::size_t free_space(const Page &page) {
  return read_u16(page, 6) - (8 + 4 * slot_count(page));
}

bool is_live(const Page &page, std::size_t slot_id) {
  if (slot_id >= slot_count(page))
    throw std::out_of_range("Slot is out of range!");

  return read_u16(page, slot_position(slot_id)) != 0;
}

void initialize(Page &page, std::uint32_t page_id) {
  write_u32(page, 0, page_id);
  write_u16(page, 4, 0);
  write_u16(page, 6, 4096);
}

std::span<const char> get(const Page &page, std::size_t slot_id) {
  if (!is_live(page, slot_id))
    throw std::runtime_error("Slot has been deleted!");

  auto position = slot_position(slot_id);
  auto offset = read_u16(page, position);
  auto length = read_u16(page, position + 2);

  return {page.data() + offset, length};
}

std::optional<std::size_t> insert(Page &page, std::span<const char> record) {
  std::size_t count = slot_count(page);
  std::size_t free_end = read_u16(page, 6);

  std::size_t available = free_space(page);
  if (available < 4 || available - 4 < record.size())
    return std::nullopt;

  std::size_t offset = free_end - record.size();
  std::copy(record.begin(), record.end(), page.data() + offset);

  std::size_t position = slot_position(count);
  // On a valid 4096-byte page, these values all fit in 16 bits.
  write_u16(page, position, static_cast<std::uint16_t>(offset));
  write_u16(page, position + 2, static_cast<std::uint16_t>(record.size()));

  write_u16(page, 4, static_cast<std::uint16_t>(count + 1));
  write_u16(page, 6, static_cast<std::uint16_t>(offset));

  return count;
}

void kill(Page &page, std::size_t slot_id) {
  if (!is_live(page, slot_id))
    throw std::runtime_error("Slot has already been deleted!");

  std::size_t position = slot_position(slot_id);
  write_u16(page, position, 0);
  write_u16(page, position + 2, 0);
}

void initialize_directory(Page &page, std::uint32_t page_id) {
  write_u32(page, 0, page_id);
  write_u32(page, 4, 0xFFFFFFFF);
  write_u16(page, 8, 0);
}

std::size_t directory_entry_count(const Page &page) {
  return read_u16(page, 8);
}

std::uint32_t next_directory(const Page &page) { return read_u32(page, 4); }

void set_next_directory(Page &page, std::uint32_t page_id) {
  write_u32(page, 4, page_id);
}

std::size_t directory_entry_position(std::size_t index) {
  return 10 + 6 * index;
}

std::size_t get_page_id(const Page &page, std::size_t index) {
  if (index >= directory_entry_count(page))
    throw std::out_of_range("Index larger than directory count!");

  return read_u32(page, directory_entry_position(index));
}

std::size_t get_page_free_bytes(const Page &page, std::size_t index) {
  if (index >= directory_entry_count(page))
    throw std::out_of_range("Index larger than directory count!");

  return read_u16(page, directory_entry_position(index) + 4);
}

void update_free_bytes(Page &page, std::size_t index,
                       std::uint16_t free_bytes) {
  if (index >= directory_entry_count(page))
    throw std::out_of_range("Index larger than directory count!");

  write_u16(page, directory_entry_position(index) + 4, free_bytes);
}

void append_entry(Page &page, std::uint32_t page_id, std::uint16_t free_bytes) {
  auto count = directory_entry_count(page);
  if (count >= 681)
    throw std::out_of_range("Directory page is full!");

  std::size_t position = directory_entry_position(count);
  write_u32(page, position, page_id);
  write_u16(page, position + 4, free_bytes);
  write_u16(page, 8, static_cast<std::uint16_t>(count + 1));
}

} // namespace vql::storage::help_me_god

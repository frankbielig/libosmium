#ifndef OSMIUM_IO_COMPRESSION_HPP
#define OSMIUM_IO_COMPRESSION_HPP

/*

This file is part of Osmium (https://osmcode.org/libosmium).

Copyright 2013-2021 Jochen Topf <jochen@topf.org> and others (see README).

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

*/

#include <osmium/io/detail/read_write.hpp>
#include <osmium/io/error.hpp>
#include <osmium/io/file_compression.hpp>
#include <osmium/io/writer_options.hpp>
#include <osmium/util/file.hpp>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

namespace osmium {

    namespace io {

        class Compressor {

            fsync m_fsync;

        protected:

            bool do_fsync() const noexcept {
                return m_fsync == fsync::yes;
            }

        public:

            explicit Compressor(const fsync sync) noexcept :
                m_fsync(sync) {
            }

            Compressor(const Compressor&) = default;
            Compressor& operator=(const Compressor&) = default;

            Compressor(Compressor&&) noexcept = default;
            Compressor& operator=(Compressor&&) noexcept = default;

            virtual ~Compressor() noexcept = default;

            virtual void write(const std::string& data) = 0;

            virtual void close() = 0;

            virtual std::size_t file_size() const {
                return 0;
            }

        }; // class Compressor

        class Decompressor {

            std::atomic<std::size_t> m_file_size{0};
            std::atomic<std::size_t> m_offset{0};

            std::atomic_bool m_want_buffered_pages_removed{false};

        public:

            enum {
                input_buffer_size = 1024U * 1024U
            };

            Decompressor() = default;

            Decompressor(const Decompressor&) = delete;
            Decompressor& operator=(const Decompressor&) = delete;

            Decompressor(Decompressor&&) = delete;
            Decompressor& operator=(Decompressor&&) = delete;

            virtual ~Decompressor() noexcept = default;

            virtual std::string read() = 0;

            virtual void close() = 0;

            virtual bool is_real() const noexcept {
                return true;
            }

            std::size_t file_size() const noexcept {
                return m_file_size;
            }

            void set_file_size(const std::size_t size) noexcept {
                m_file_size = size;
            }

            std::size_t offset() const noexcept {
                return m_offset;
            }

            void set_offset(const std::size_t offset) noexcept {
                m_offset = offset;
            }

            bool want_buffered_pages_removed() const noexcept {
                return m_want_buffered_pages_removed;
            }

            void set_want_buffered_pages_removed(bool value) noexcept {
                m_want_buffered_pages_removed = value;
            }

        }; // class Decompressor

        /**
         * This singleton factory class is used to register compression
         * algorithms used for reading and writing OSM files.
         *
         * For each algorithm we store two functions that construct
         * a compressor and decompressor object, respectively.
         */
        class CompressionFactory {

        public:

            using create_compressor_type          = std::function<osmium::io::Compressor*(int, fsync)>;
            using create_decompressor_type_fd     = std::function<osmium::io::Decompressor*(int)>;
            using create_decompressor_type_buffer = std::function<osmium::io::Decompressor*(const char*, std::size_t)>;

        private:

            using callbacks_type = std::tuple<create_compressor_type,
                                              create_decompressor_type_fd,
                                              create_decompressor_type_buffer>;

            using compression_map_type = std::map<const osmium::io::file_compression, callbacks_type>;

            compression_map_type m_callbacks;

            CompressionFactory() = default;

            const callbacks_type& find_callbacks(const osmium::io::file_compression compression) const {
                const auto it = m_callbacks.find(compression);

                if (it != m_callbacks.end()) {
                    return it->second;
                }

                std::string error_message{"Support for compression '"};
                error_message += as_string(compression);
                error_message += "' not compiled into this binary";
                throw unsupported_file_format_error{error_message};
            }

        public:

            CompressionFactory(const CompressionFactory&) = delete;
            CompressionFactory& operator=(const CompressionFactory&) = delete;

            CompressionFactory(CompressionFactory&&) = delete;
            CompressionFactory& operator=(CompressionFactory&&) = delete;

            ~CompressionFactory() noexcept = default;

            static CompressionFactory& instance() {
                static CompressionFactory factory;
                return factory;
            }

            bool register_compression(
                osmium::io::file_compression compression,
                const create_compressor_type& create_compressor,
                const create_decompressor_type_fd& create_decompressor_fd,
                const create_decompressor_type_buffer& create_decompressor_buffer) {

                compression_map_type::value_type cc{compression,
                                                    std::make_tuple(create_compressor,
                                                                    create_decompressor_fd,
                                                                    create_decompressor_buffer)};

                return m_callbacks.insert(cc).second;
            }

            template <typename... TArgs>
            std::unique_ptr<osmium::io::Compressor> create_compressor(const osmium::io::file_compression compression, TArgs&&... args) const {
                const auto callbacks = find_callbacks(compression);
                return std::unique_ptr<osmium::io::Compressor>(std::get<0>(callbacks)(std::forward<TArgs>(args)...));
            }

            std::unique_ptr<osmium::io::Decompressor> create_decompressor(const osmium::io::file_compression compression, const int fd) const {
                const auto callbacks = find_callbacks(compression);
                auto p = std::unique_ptr<osmium::io::Decompressor>(std::get<1>(callbacks)(fd));
                p->set_file_size(osmium::file_size(fd));
                return p;
            }

            std::unique_ptr<osmium::io::Decompressor> create_decompressor(const osmium::io::file_compression compression, const char* buffer, const std::size_t size) const {
                const auto callbacks = find_callbacks(compression);
                return std::unique_ptr<osmium::io::Decompressor>(std::get<2>(callbacks)(buffer, size));
            }

        }; // class CompressionFactory

        class NoCompressor final : public Compressor {

            std::size_t m_file_size = 0;
            int m_fd;

        public:

            NoCompressor(const int fd, const fsync sync) :
                Compressor(sync),
                m_fd(fd) {
            }

            NoCompressor(const NoCompressor&) = delete;
            NoCompressor& operator=(const NoCompressor&) = delete;

            NoCompressor(NoCompressor&&) = delete;
            NoCompressor& operator=(NoCompressor&&) = delete;

            ~NoCompressor() noexcept override {
                try {
                    close();
                } catch (...) {
                    // Ignore any exceptions because destructor must not throw.
                }
            }

            void write(const std::string& data) override {
                osmium::io::detail::reliable_write(m_fd, data.data(), data.size());
                m_file_size += data.size();
            }

            void close() override {
                if (m_fd >= 0) {
                    const int fd = m_fd;
                    m_fd = -1;

                    // Do not sync or close stdout
                    if (fd == 1) {
                        return;
                    }

                    if (do_fsync()) {
                        osmium::io::detail::reliable_fsync(fd);
                    }
                    osmium::io::detail::reliable_close(fd);
                }
            }

            std::size_t file_size() const override {
                return m_file_size;
            }

        }; // class NoCompressor

        /**
         * The DummyDecompressor is used when reading PBF files. In that
         * case the PBFParser class is responsible for reading from the
         * file itself, and the DummyDecompressor does nothing.
         */
        class DummyDecompressor final : public Decompressor {
        public:

            DummyDecompressor() = default;

            DummyDecompressor(const DummyDecompressor&) = delete;
            DummyDecompressor& operator=(const DummyDecompressor&) = delete;

            DummyDecompressor(DummyDecompressor&&) = delete;
            DummyDecompressor& operator=(DummyDecompressor&&) = delete;

            ~DummyDecompressor() noexcept override = default;

            std::string read() override {
                return {};
            }

            void close() override {
            }

            bool is_real() const noexcept override {
                return false;
            }

        }; // class DummyDecompressor

        class NoDecompressor final : public Decompressor {

            int m_fd = -1;
            const char* m_buffer = nullptr;
            std::size_t m_buffer_size = 0;
            std::size_t m_offset = 0;

        public:

            explicit NoDecompressor(const int fd) :
                m_fd(fd) {
            }

            NoDecompressor(const char* buffer, const std::size_t size) :
                m_buffer(buffer),
                m_buffer_size(size) {
            }

            NoDecompressor(const NoDecompressor&) = delete;
            NoDecompressor& operator=(const NoDecompressor&) = delete;

            NoDecompressor(NoDecompressor&&) = delete;
            NoDecompressor& operator=(NoDecompressor&&) = delete;

            ~NoDecompressor() noexcept override {
                try {
                    close();
                } catch (...) {
                    // Ignore any exceptions because destructor must not throw.
                }
            }

            std::string read() override {
                std::string buffer;

                if (m_buffer) {
                    if (m_buffer_size != 0) {
                        const std::size_t size = m_buffer_size;
                        m_buffer_size = 0;
                        buffer.append(m_buffer, size);
                    }
                } else {
                    buffer.resize(osmium::io::Decompressor::input_buffer_size);
                    if (want_buffered_pages_removed()) {
                        osmium::io::detail::remove_buffered_pages(m_fd, m_offset);
                    }
                    const auto nread = detail::reliable_read(m_fd, &*buffer.begin(), osmium::io::Decompressor::input_buffer_size);
                    buffer.resize(std::string::size_type(nread));
                }

                m_offset += buffer.size();
                set_offset(m_offset);

                return buffer;
            }

            void close() override {
                if (m_fd >= 0) {
                    if (want_buffered_pages_removed()) {
                        osmium::io::detail::remove_buffered_pages(m_fd);
                    }
                    const int fd = m_fd;
                    m_fd = -1;
                    osmium::io::detail::reliable_close(fd);
                }
            }

        }; // class NoDecompressor

        namespace detail {

            // we want the register_compression() function to run, setting
            // the variable is only a side-effect, it will never be used
            const bool registered_no_compression = osmium::io::CompressionFactory::instance().register_compression(osmium::io::file_compression::none,
                [](const int fd, const fsync sync) { return new osmium::io::NoCompressor{fd, sync}; },
                [](const int fd) { return new osmium::io::NoDecompressor{fd}; },
                [](const char* buffer, std::size_t size) { return new osmium::io::NoDecompressor{buffer, size}; }
            );

            // dummy function to silence the unused variable warning from above
            inline bool get_registered_no_compression() noexcept {
                return registered_no_compression;
            }

        } // namespace detail

    } // namespace io

} // namespace osmium

#endif // OSMIUM_IO_COMPRESSION_HPP

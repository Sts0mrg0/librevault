/* Copyright (C) 2015 Alexander Shishenko <GamePad64@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "AutoIndexer.h"
#include "IgnoreList.h"
#include "Indexer.h"
#include "FSFolder.h"
#include "../../Client.h"

namespace librevault {

AutoIndexer::AutoIndexer(FSFolder& dir, Client& client) :
		Loggable(dir, "AutoIndexer"),
		dir_(dir), client_(client),
		monitor_(client_.bulk_ios()), reindex_timer_(client_.bulk_ios()), index_timer_(client_.bulk_ios()) {
	enqueue_files(full_reindex_list());

	monitor_.add_directory(dir_.path().string());
	monitor_operation();
}

void AutoIndexer::enqueue_files(const std::string& relpath) {
	std::unique_lock<std::mutex> lk(index_queue_mtx_);
	index_queue_.insert(relpath);
	bump_timer();
}

void AutoIndexer::enqueue_files(const std::set<std::string>& relpath) {
	std::unique_lock<std::mutex> lk(index_queue_mtx_);
	index_queue_.insert(relpath.begin(), relpath.end());
	bump_timer();
}

void AutoIndexer::prepare_file_assemble(bool with_removal, const std::string& relpath) {
	unsigned skip_events = 2;	// REMOVED, RENAMED (NEW NAME), MODIFIED
	if(with_removal) skip_events++;
	//unsigned skip_events = 0;

	for(unsigned i = 0; i < skip_events; i++)
		prepared_assemble_.insert(relpath);
}

void AutoIndexer::prepare_dir_assemble(bool with_removal, const std::string& relpath) {
	unsigned skip_events = 1;	// ADDED
	skip_events += with_removal ? 1 : 0;

	for(unsigned i = 0; i < skip_events; i++)
		prepared_assemble_.insert(relpath);
}

void AutoIndexer::prepare_deleted_assemble(const std::string& relpath) {
	unsigned skip_events = 1;	// REMOVED or UNKNOWN

	for(unsigned i = 0; i < skip_events; i++)
		prepared_assemble_.insert(relpath);
}

std::set<std::string> AutoIndexer::short_reindex_list() {
	std::set<std::string> file_list;

	// Files present in the file system
	for(auto dir_entry_it = fs::recursive_directory_iterator(dir_.path()); dir_entry_it != fs::recursive_directory_iterator(); dir_entry_it++){
		auto relpath = dir_.normalize_path(dir_entry_it->path());

		if(!dir_.ignore_list->is_ignored(relpath)) file_list.insert(relpath);
	}

	return file_list;
}

std::set<std::string> AutoIndexer::full_reindex_list() {
	std::set<std::string> file_list = short_reindex_list();

	// Files present in index (files added from here will be marked as DELETED)
	for(auto smeta : dir_.index->get_meta()) {
		auto& meta = smeta.meta();
		if(meta.meta_type() != Meta::DELETED){
			file_list.insert(meta.path(dir_.secret()));
		}
	}

	return file_list;
}

void AutoIndexer::bump_timer() {
	if(index_timer_.expires_from_now() <= std::chrono::seconds(0)){
		auto exp_timeout = std::chrono::milliseconds(dir_.params().index_event_timeout);

		index_timer_.expires_from_now(exp_timeout);
		index_timer_.async_wait(std::bind(&AutoIndexer::monitor_index, this, std::placeholders::_1));
	}
}

void AutoIndexer::monitor_operation() {
	monitor_.async_monitor([this](boost::system::error_code ec, boost::asio::dir_monitor_event ev){
		if(ec == boost::asio::error::operation_aborted) return;

		monitor_handle(ev);
		monitor_operation();
	});
}

void AutoIndexer::monitor_handle(const boost::asio::dir_monitor_event& ev) {
	switch(ev.type){
	case boost::asio::dir_monitor_event::added:
	case boost::asio::dir_monitor_event::modified:
	case boost::asio::dir_monitor_event::renamed_old_name:
	case boost::asio::dir_monitor_event::renamed_new_name:
	case boost::asio::dir_monitor_event::removed:
	case boost::asio::dir_monitor_event::null:
	{
		std::string relpath = dir_.normalize_path(ev.path);

		auto prepared_assemble_it = prepared_assemble_.find(relpath);
		if(prepared_assemble_it != prepared_assemble_.end()) {
			prepared_assemble_.erase(prepared_assemble_it);
			return;
			// FIXME: "prepares" is a dirty hack. It must be EXTERMINATED!
		}

		if(!dir_.ignore_list->is_ignored(relpath)){
			log_->debug() << "[dir_monitor] " << ev;
			enqueue_files(relpath);
		}
	}
	default: break;
	}
}

void AutoIndexer::monitor_index(const boost::system::error_code& ec) {
	if(ec == boost::asio::error::operation_aborted) return;

	std::set<std::string> index_queue;
	index_queue_mtx_.lock();
	index_queue.swap(index_queue_);
	index_queue_mtx_.unlock();

	dir_.indexer->async_index(index_queue);
}

} /* namespace librevault */
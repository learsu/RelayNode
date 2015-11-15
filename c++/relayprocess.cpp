#include "preinclude.h"

#include "relayprocess.h"

#include "crypto/sha2.h"

#include <string.h>

std::shared_ptr<std::vector<unsigned char> > RelayNodeCompressor::get_relay_transaction(const std::shared_ptr<std::vector<unsigned char> >& tx) {
	std::lock_guard<std::mutex> lock(mutex);

	if (send_tx_cache.contains(tx))
		return std::shared_ptr<std::vector<unsigned char> >();

	if (!useOldFlags) {
		if (tx->size() > MAX_RELAY_TRANSACTION_BYTES)
			return std::shared_ptr<std::vector<unsigned char> >();
		send_tx_cache.add(tx, tx->size());

	}

	if (useOldFlags) {
		if (tx->size() > OLD_MAX_RELAY_TRANSACTION_BYTES &&
				(send_tx_cache.flagCount() >= OLD_MAX_EXTRA_OVERSIZE_TRANSACTIONS || tx->size() > OLD_MAX_RELAY_OVERSIZE_TRANSACTION_BYTES))
			return std::shared_ptr<std::vector<unsigned char> >();
		send_tx_cache.add(tx, tx->size() > OLD_MAX_RELAY_TRANSACTION_BYTES);
	}

	return tx_to_msg(tx);
}

void RelayNodeCompressor::reset() {
	std::lock_guard<std::mutex> lock(mutex);

	recv_tx_cache.clear();
	send_tx_cache.clear();
}

bool RelayNodeCompressor::check_recv_tx(uint32_t tx_size) {
	return (!useOldFlags && tx_size <= MAX_RELAY_TRANSACTION_BYTES) ||
			(useOldFlags && (tx_size <= OLD_MAX_RELAY_TRANSACTION_BYTES || (recv_tx_cache.flagCount() < OLD_MAX_EXTRA_OVERSIZE_TRANSACTIONS && tx_size <= OLD_MAX_RELAY_OVERSIZE_TRANSACTION_BYTES)));
}

bool RelayNodeCompressor::maybe_recv_tx_of_size(uint32_t tx_size, bool debug_print) {
	std::lock_guard<std::mutex> lock(mutex);

	if (!check_recv_tx(tx_size)) {
		if (debug_print)
			printf("Freely relayed tx of size %u, with %lu oversize txn already present\n", tx_size, (long unsigned)recv_tx_cache.flagCount());
		return false;
	}
	return true;
}

void RelayNodeCompressor::recv_tx(std::shared_ptr<std::vector<unsigned char > > tx) {
	std::lock_guard<std::mutex> lock(mutex);

	uint32_t tx_size = tx.get()->size();
	assert(check_recv_tx(tx_size));
	recv_tx_cache.add(tx, useOldFlags ? tx_size > OLD_MAX_RELAY_TRANSACTION_BYTES : tx_size);
}

void RelayNodeCompressor::for_each_sent_tx(const std::function<void (const std::shared_ptr<std::vector<unsigned char> >&)> callback) {
	std::lock_guard<std::mutex> lock(mutex);
	send_tx_cache.for_all_txn(callback);
}

bool RelayNodeCompressor::block_sent(std::vector<unsigned char>& hash) {
	std::lock_guard<std::mutex> lock(mutex);
	return blocksAlreadySeen.insert(hash).second;
}

uint32_t RelayNodeCompressor::blocks_sent() {
	std::lock_guard<std::mutex> lock(mutex);
	return blocksAlreadySeen.size();
}

bool RelayNodeCompressor::was_tx_sent(const unsigned char* txhash) {
	std::lock_guard<std::mutex> lock(mutex);
	return send_tx_cache.contains(txhash);
}

class MerkleTreeBuilder {
private:
	std::vector<unsigned char> hashlist;
public:
	MerkleTreeBuilder(uint32_t tx_count) : hashlist(tx_count * 32) {}
	inline unsigned char* getTxHashLoc(uint32_t tx) { return &hashlist[tx * 32]; }
	bool merkleRootMatches(const unsigned char* match) {
		uint32_t txcount = hashlist.size() / 32;
		uint32_t stepCount = 1, lastMax = txcount - 1;
		for (uint32_t rowSize = txcount; rowSize > 1; rowSize = (rowSize + 1) / 2) {
			if (!memcmp(&hashlist[32 * (lastMax - stepCount)], &hashlist[32 * lastMax], 32))
				return false;

			for (uint32_t i = 0; i < rowSize; i += 2) {
				assert(i*stepCount < txcount && lastMax < txcount);
				double_sha256_two_32_inputs(&hashlist[32 * i*stepCount], &hashlist[32 * std::min((i + 1)*stepCount, lastMax)], &hashlist[32 * i*stepCount]);
			}
			lastMax = ((rowSize - 1) & 0xfffffffe) * stepCount;
			stepCount *= 2;
		}
		return !memcmp(match, &hashlist[0], 32);
	}
};

std::tuple<std::shared_ptr<std::vector<unsigned char> >, const char*> RelayNodeCompressor::maybe_compress_block(const std::vector<unsigned char>& hash, const std::vector<unsigned char>& block, bool check_merkle) {
	std::lock_guard<std::mutex> lock(mutex);
	FASLockHint faslock(send_tx_cache);

	if (check_merkle && (hash[31] != 0 || hash[30] != 0 || hash[29] != 0 || hash[28] != 0 || hash[27] != 0 || hash[26] != 0 || hash[25] != 0))
		return std::make_tuple(std::shared_ptr<std::vector<unsigned char> >(), "BAD_WORK");

	if (blocksAlreadySeen.count(hash))
		return std::make_tuple(std::shared_ptr<std::vector<unsigned char> >(), "SEEN");

	auto compressed_block = std::make_shared<std::vector<unsigned char> >();
	compressed_block->reserve(1100000);
	struct relay_msg_header header;

	try {
		std::vector<unsigned char>::const_iterator readit = block.begin();
		move_forward(readit, sizeof(struct bitcoin_msg_header), block.end());
		move_forward(readit, 4, block.end());
#ifndef TEST_DATA
		int32_t block_version = ((*(readit-1) << 24) | (*(readit-2) << 16) | (*(readit-3) << 8) | *(readit-4));
		if (block_version < 4)
			return std::make_tuple(std::make_shared<std::vector<unsigned char> >(), "SMALL_VERSION");
#endif

		move_forward(readit, 32, block.end());
		auto merkle_hash_it = readit;
		move_forward(readit, 80 - (4 + 32), block.end());

		uint64_t txcount = read_varint(readit, block.end());
		if (txcount < 1 || txcount > 100000)
			return std::make_tuple(std::make_shared<std::vector<unsigned char> >(), "TXCOUNT_RANGE");

		header.magic = RELAY_MAGIC_BYTES;
		header.type = BLOCK_TYPE;
		header.length = htonl(txcount);
		compressed_block->insert(compressed_block->end(), (unsigned char*)&header, ((unsigned char*)&header) + sizeof(header));
		compressed_block->insert(compressed_block->end(), block.begin() + sizeof(struct bitcoin_msg_header), block.begin() + 80 + sizeof(struct bitcoin_msg_header));

		MerkleTreeBuilder merkleTree(check_merkle ? txcount : 0);

		for (uint32_t i = 0; i < txcount; i++) {
			std::vector<unsigned char>::const_iterator txstart = readit;

			move_forward(readit, 4, block.end());

			uint64_t txins = read_varint(readit, block.end());
			for (uint64_t j = 0; j < txins; j++) {
				move_forward(readit, 36, block.end());
				move_forward(readit, read_varint(readit, block.end()) + 4, block.end());
			}

			uint64_t txouts = read_varint(readit, block.end());
			for (uint64_t j = 0; j < txouts; j++) {
				move_forward(readit, 8, block.end());
				move_forward(readit, read_varint(readit, block.end()), block.end());
			}

			move_forward(readit, 4, block.end());

			int index = send_tx_cache.remove(txstart, readit);

			__builtin_prefetch(&(*readit), 0);
			__builtin_prefetch(&(*readit) + 64, 0);
			__builtin_prefetch(&(*readit) + 128, 0);
			__builtin_prefetch(&(*readit) + 196, 0);
			__builtin_prefetch(&(*readit) + 256, 0);

			if (check_merkle)
				double_sha256(&(*txstart), merkleTree.getTxHashLoc(i), readit - txstart);

			if (index < 0) {
				compressed_block->push_back(0xff);
				compressed_block->push_back(0xff);

				uint32_t txlen = readit - txstart;
				compressed_block->push_back((txlen >> 16) & 0xff);
				compressed_block->push_back((txlen >>  8) & 0xff);
				compressed_block->push_back((txlen      ) & 0xff);

				compressed_block->insert(compressed_block->end(), txstart, readit);
			} else {
				compressed_block->push_back((index >> 8) & 0xff);
				compressed_block->push_back((index     ) & 0xff);
			}
		}

		if (check_merkle && !merkleTree.merkleRootMatches(&(*merkle_hash_it)))
			return std::make_tuple(std::make_shared<std::vector<unsigned char> >(), "INVALID_MERKLE");
	} catch(read_exception) {
		return std::make_tuple(std::make_shared<std::vector<unsigned char> >(), "INVALID_SIZE");
	}

	if (!blocksAlreadySeen.insert(hash).second)
		return std::make_tuple(std::shared_ptr<std::vector<unsigned char> >(), "MUTEX_BROKEN???");

	return std::make_tuple(compressed_block, (const char*)NULL);
}

struct IndexVector {
	uint16_t index;
	std::vector<unsigned char> data;
};
struct IndexPtr {
	uint16_t index;
	size_t pos;
	IndexPtr(uint16_t index_in, size_t pos_in) : index(index_in), pos(pos_in) {}
	bool operator< (const IndexPtr& o) const { return index < o.index; }
};

void tweak_sort(std::vector<IndexPtr>& ptrs, size_t start, size_t end) {
	if (start + 1 >= end)
		return;
	size_t split = (end - start) / 2 + start;
	tweak_sort(ptrs, start, split);
	tweak_sort(ptrs, split, end);

	size_t j = 0, k = split;
	std::vector<IndexPtr> left(ptrs.begin() + start, ptrs.begin() + split);
	for (size_t i = start; i < end; i++) {
		if (j < left.size() && (k >= end || left[j].index - (k - split) <= ptrs[k].index)) {
			ptrs[i] = left[j++];
			ptrs[i].index -= (k - split);
		} else
			ptrs[i] = ptrs[k++];
	}
}

class DecompressState {
public:
	const bool check_merkle;
	const uint32_t tx_count;

	uint32_t wire_bytes = 4*3;
	std::shared_ptr<std::vector<unsigned char> > block;
	std::shared_ptr<std::vector<unsigned char> > fullhashptr;

	MerkleTreeBuilder merkleTree;
	std::vector<IndexVector> txn_data;
	std::vector<IndexPtr> txn_ptrs;

public:
	DecompressState(bool check_merkle_in, uint32_t tx_count_in);
	const char* do_decompress(std::function<ssize_t(char*, size_t)>& read_all);
};

DecompressState::DecompressState(bool check_merkle_in, uint32_t tx_count_in) :
		check_merkle(check_merkle_in), tx_count(tx_count_in),
		wire_bytes(4*3),
		block(std::make_shared<std::vector<unsigned char> >(sizeof(bitcoin_msg_header) + 80)),
		fullhashptr(std::make_shared<std::vector<unsigned char> >(32)),
		merkleTree(check_merkle ? tx_count : 1),
		txn_data(tx_count_in) {
	block->reserve(1000000 + sizeof(bitcoin_msg_header));
	txn_ptrs.reserve(tx_count);
}


std::tuple<uint32_t, std::shared_ptr<std::vector<unsigned char> >, const char*, std::shared_ptr<std::vector<unsigned char> > > RelayNodeCompressor::decompress_relay_block(std::function<ssize_t(char*, size_t)>& read_all, uint32_t message_size, bool check_merkle) {
	std::lock_guard<std::mutex> lock(mutex);
	FASLockHint faslock(recv_tx_cache);

	if (message_size > 100000)
		return std::make_tuple(0, std::shared_ptr<std::vector<unsigned char> >(NULL), "got a BLOCK message with far too many transactions", std::shared_ptr<std::vector<unsigned char> >(NULL));

	DecompressState state(check_merkle, message_size);
	const char* err = do_decompress(state, read_all);
	if (err)
		return std::make_tuple(0, std::shared_ptr<std::vector<unsigned char> >(NULL), err, std::shared_ptr<std::vector<unsigned char> >(NULL));
	return std::make_tuple(state.wire_bytes, state.block, (const char*) NULL, state.fullhashptr);
}

const char* RelayNodeCompressor::do_decompress(DecompressState& state, std::function<ssize_t(char*, size_t)>& read_all) {
	if (read_all((char*)&(*state.block)[sizeof(bitcoin_msg_header)], 80) != 80)
		return "failed to read block header";

#ifndef TEST_DATA
	int32_t block_version = (((*state.block)[sizeof(bitcoin_msg_header) + 3] << 24) | ((*state.block)[sizeof(bitcoin_msg_header) + 2] << 16) | ((*state.block)[sizeof(bitcoin_msg_header) + 1] << 8) | (*state.block)[sizeof(bitcoin_msg_header)]);
	if (block_version < 4)
		return "block had version < 4";
#endif

	getblockhash(*state.fullhashptr.get(), *state.block, sizeof(struct bitcoin_msg_header));
	blocksAlreadySeen.insert(*state.fullhashptr.get());

	if (state.check_merkle && ((*state.fullhashptr)[31] != 0 || (*state.fullhashptr)[30] != 0 || (*state.fullhashptr)[29] != 0 || (*state.fullhashptr)[28] != 0 || (*state.fullhashptr)[27] != 0 || (*state.fullhashptr)[26] != 0 || (*state.fullhashptr)[25] != 0))
		return "block hash did not meet minimum difficulty target";

	auto vartxcount = varint(state.tx_count);
	state.block->insert(state.block->end(), vartxcount.begin(), vartxcount.end());

	for (uint32_t i = 0; i < state.tx_count; i++) {
		uint16_t index;
		if (read_all((char*)&index, 2) != 2)
			return "failed to read tx index";
		index = ntohs(index);
		state.wire_bytes += 2;

		state.txn_data[i].index = index;

		if (index == 0xffff) {
			union intbyte {
				uint32_t i;
				char c[4];
			} tx_size {0};

			if (read_all(tx_size.c + 1, 3) != 3)
				return "failed to read tx length";
			tx_size.i = ntohl(tx_size.i);

			if (tx_size.i > 1000000)
				return "got unreasonably large tx";

			state.txn_data[i].data.resize(tx_size.i);
			if (read_all((char*)&(state.txn_data[i].data[0]), tx_size.i) != int64_t(tx_size.i))
				return "failed to read transaction data";
			state.wire_bytes += 3 + tx_size.i;

			if (state.check_merkle)
				double_sha256(&(state.txn_data[i].data[0]), state.merkleTree.getTxHashLoc(i), tx_size.i);
		} else
			state.txn_ptrs.emplace_back(index, i);
	}

	tweak_sort(state.txn_ptrs, 0, state.txn_ptrs.size());
#ifndef NDEBUG
	int32_t last = -1;
#endif
	for (size_t i = 0; i < state.txn_ptrs.size(); i++) {
		const IndexPtr& ptr = state.txn_ptrs[i];
		assert(last <= int(ptr.index) && (last = ptr.index) != -1);

		if (!recv_tx_cache.remove(ptr.index, state.txn_data[ptr.pos].data, state.merkleTree.getTxHashLoc(state.check_merkle ? ptr.pos : 0)))
			return "failed to find referenced transaction";
	}

	for (uint32_t i = 0; i < state.tx_count; i++)
		state.block->insert(state.block->end(), state.txn_data[i].data.begin(), state.txn_data[i].data.end());

	if (state.check_merkle && !state.merkleTree.merkleRootMatches(&(*state.block)[4 + 32 + sizeof(bitcoin_msg_header)]))
		return "merkle tree root did not match";

	return NULL;
}

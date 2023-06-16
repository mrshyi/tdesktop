/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/stories/info_stories_provider.h"

#include "info/media/info_media_widget.h"
#include "info/media/info_media_list_section.h"
#include "info/info_controller.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_stories_ids.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "history/history.h"
#include "core/application.h"
#include "storage/storage_shared_media.h"
#include "layout/layout_selection.h"
#include "styles/style_info.h"

namespace Info::Stories {
namespace {

using namespace Media;

constexpr auto kPreloadedScreensCount = 4;
constexpr auto kPreloadedScreensCountFull
	= kPreloadedScreensCount + 1 + kPreloadedScreensCount;

[[nodiscard]] int MinStoryHeight(int width) {
	auto itemsLeft = st::infoMediaSkip;
	auto itemsInRow = (width - itemsLeft)
		/ (st::infoMediaMinGridSize + st::infoMediaSkip);
	return (st::infoMediaMinGridSize + st::infoMediaSkip) / itemsInRow;
}

} // namespace

Provider::Provider(not_null<AbstractController*> controller)
: _controller(controller)
, _peer(controller->key().storiesPeer())
, _history(_peer->owner().history(_peer))
, _tab(controller->key().storiesTab()) {
	style::PaletteChanged(
	) | rpl::start_with_next([=] {
		for (auto &layout : _layouts) {
			layout.second.item->invalidateCache();
		}
	}, _lifetime);
}

Type Provider::type() {
	return Type::PhotoVideo;
}

bool Provider::hasSelectRestriction() {
	return false;
}

rpl::producer<bool> Provider::hasSelectRestrictionChanges() {
	return rpl::never<bool>();
}

bool Provider::sectionHasFloatingHeader() {
	return false;
}

QString Provider::sectionTitle(not_null<const BaseLayout*> item) {
	return QString();
}

bool Provider::sectionItemBelongsHere(
		not_null<const BaseLayout*> item,
		not_null<const BaseLayout*> previous) {
	return true;
}

bool Provider::isPossiblyMyItem(not_null<const HistoryItem*> item) {
	return true;
}

std::optional<int> Provider::fullCount() {
	return _slice.fullCount();
}

void Provider::restart() {
	_layouts.clear();
	_aroundId = kDefaultAroundId;
	_idsLimit = kMinimalIdsLimit;
	_slice = Data::StoriesIdsSlice();
	refreshViewer();
}

void Provider::checkPreload(
		QSize viewport,
		not_null<BaseLayout*> topLayout,
		not_null<BaseLayout*> bottomLayout,
		bool preloadTop,
		bool preloadBottom) {
	const auto visibleWidth = viewport.width();
	const auto visibleHeight = viewport.height();
	const auto preloadedHeight = kPreloadedScreensCountFull * visibleHeight;
	const auto minItemHeight = MinStoryHeight(visibleWidth);
	const auto preloadedCount = preloadedHeight / minItemHeight;
	const auto preloadIdsLimitMin = (preloadedCount / 2) + 1;
	const auto preloadIdsLimit = preloadIdsLimitMin
		+ (visibleHeight / minItemHeight);
	const auto after = _slice.skippedAfter();
	const auto topLoaded = after && (*after == 0);
	const auto before = _slice.skippedBefore();
	const auto bottomLoaded = before && (*before == 0);

	const auto minScreenDelta = kPreloadedScreensCount
		- kPreloadIfLessThanScreens;
	const auto minIdDelta = (minScreenDelta * visibleHeight)
		/ minItemHeight;
	const auto preloadAroundItem = [&](not_null<BaseLayout*> layout) {
		auto preloadRequired = false;
		const auto id = StoryIdFromMsgId(layout->getItem()->id);
		if (!preloadRequired) {
			preloadRequired = (_idsLimit < preloadIdsLimitMin);
		}
		if (!preloadRequired) {
			auto delta = _slice.distance(_aroundId, id);
			Assert(delta != std::nullopt);
			preloadRequired = (qAbs(*delta) >= minIdDelta);
		}
		if (preloadRequired) {
			_idsLimit = preloadIdsLimit;
			_aroundId = id;
			refreshViewer();
		}
	};

	if (preloadTop && !topLoaded) {
		preloadAroundItem(topLayout);
	} else if (preloadBottom && !bottomLoaded) {
		preloadAroundItem(bottomLayout);
	}
}

void Provider::setSearchQuery(QString query) {
}

void Provider::refreshViewer() {
	_viewerLifetime.destroy();
	const auto idForViewer = _aroundId;
	const auto session = &_peer->session();
	auto ids = (_tab == Tab::Saved)
		? Data::SavedStoriesIds(_peer, idForViewer, _idsLimit)
		: Data::ArchiveStoriesIds(session, idForViewer, _idsLimit);
	std::move(
		ids
	) | rpl::start_with_next([=](Data::StoriesIdsSlice &&slice) {
		if (!slice.fullCount()) {
			// Don't display anything while full count is unknown.
			return;
		}
		_slice = std::move(slice);
		if (const auto nearest = _slice.nearest(idForViewer)) {
			_aroundId = *nearest;
		}
		_refreshed.fire({});
	}, _viewerLifetime);
}

rpl::producer<> Provider::refreshed() {
	return _refreshed.events();
}

std::vector<ListSection> Provider::fillSections(
		not_null<Overview::Layout::Delegate*> delegate) {
	markLayoutsStale();
	const auto guard = gsl::finally([&] { clearStaleLayouts(); });

	auto result = std::vector<ListSection>();
	auto section = ListSection(Type::PhotoVideo, sectionDelegate());
	auto count = _slice.size();
	for (auto i = count; i != 0;) {
		const auto storyId = _slice[--i];
		if (const auto layout = getLayout(storyId, delegate)) {
			if (!section.addItem(layout)) {
				section.finishSection();
				result.push_back(std::move(section));
				section = ListSection(Type::PhotoVideo, sectionDelegate());
				section.addItem(layout);
			}
		}
	}
	if (!section.empty()) {
		section.finishSection();
		result.push_back(std::move(section));
	}
	return result;
}

void Provider::markLayoutsStale() {
	for (auto &layout : _layouts) {
		layout.second.stale = true;
	}
}

void Provider::clearStaleLayouts() {
	for (auto i = _layouts.begin(); i != _layouts.end();) {
		if (i->second.stale) {
			_layoutRemoved.fire(i->second.item.get());
			const auto taken = _items.take(i->first);
			i = _layouts.erase(i);
		} else {
			++i;
		}
	}
}

rpl::producer<not_null<BaseLayout*>> Provider::layoutRemoved() {
	return _layoutRemoved.events();
}

BaseLayout *Provider::lookupLayout(const HistoryItem *item) {
	return nullptr;
}

bool Provider::isMyItem(not_null<const HistoryItem*> item) {
	return IsStoryMsgId(item->id) && (item->history()->peer == _peer);
}

bool Provider::isAfter(
		not_null<const HistoryItem*> a,
		not_null<const HistoryItem*> b) {
	return (a->id < b->id);
}

void Provider::itemRemoved(not_null<const HistoryItem*> item) {
	const auto id = StoryIdFromMsgId(item->id);
	if (const auto i = _layouts.find(id); i != end(_layouts)) {
		_layoutRemoved.fire(i->second.item.get());
		_layouts.erase(i);
	}
}

BaseLayout *Provider::getLayout(
		StoryId id,
		not_null<Overview::Layout::Delegate*> delegate) {
	auto it = _layouts.find(id);
	if (it == _layouts.end()) {
		if (auto layout = createLayout(id, delegate)) {
			layout->initDimensions();
			it = _layouts.emplace(id, std::move(layout)).first;
		} else {
			return nullptr;
		}
	}
	it->second.stale = false;
	return it->second.item.get();
}

HistoryItem *Provider::ensureItem(StoryId id) {
	const auto i = _items.find(id);
	if (i != end(_items)) {
		return i->second.get();
	}
	auto item = _peer->owner().stories().resolveItem({ _peer->id, id });
	if (!item) {
		return nullptr;
	}
	return _items.emplace(id, std::move(item)).first->second.get();
}

std::unique_ptr<BaseLayout> Provider::createLayout(
		StoryId id,
		not_null<Overview::Layout::Delegate*> delegate) {
	const auto item = ensureItem(id);
	if (!item) {
		return nullptr;
	}
	const auto getPhoto = [&]() -> PhotoData* {
		if (const auto media = item->media()) {
			return media->photo();
		}
		return nullptr;
	};
	const auto getFile = [&]() -> DocumentData* {
		if (const auto media = item->media()) {
			return media->document();
		}
		return nullptr;
	};
	// #TODO stories
	const auto maybeStory = item->history()->owner().stories().lookup(
		{ item->history()->peer->id, StoryIdFromMsgId(item->id) });
	const auto spoiler = maybeStory && !(*maybeStory)->expired();

	using namespace Overview::Layout;
	if (const auto photo = getPhoto()) {
		return std::make_unique<Photo>(delegate, item, photo, spoiler);
	} else if (const auto file = getFile()) {
		return std::make_unique<Video>(delegate, item, file, spoiler);
	} else {
		return std::make_unique<Photo>(
			delegate,
			item,
			Data::MediaStory::LoadingStoryPhoto(&item->history()->owner()),
			spoiler);
	}
	return nullptr;
}

ListItemSelectionData Provider::computeSelectionData(
		not_null<const HistoryItem*> item,
		TextSelection selection) {
	auto result = ListItemSelectionData(selection);
	result.canDelete = true;
	result.canForward = item->allowsForward()
		&& (&item->history()->session() == &_controller->session());
	return result;
}

void Provider::applyDragSelection(
		ListSelectedMap &selected,
		not_null<const HistoryItem*> fromItem,
		bool skipFrom,
		not_null<const HistoryItem*> tillItem,
		bool skipTill) {
	const auto fromId = fromItem->id - (skipFrom ? 1 : 0);
	const auto tillId = tillItem->id - (skipTill ? 0 : 1);
	for (auto i = selected.begin(); i != selected.end();) {
		const auto itemId = i->first->id;
		if (itemId > fromId || itemId <= tillId) {
			i = selected.erase(i);
		} else {
			++i;
		}
	}
	for (auto &layoutItem : _layouts) {
		const auto id = StoryIdToMsgId(layoutItem.first);
		if (id <= fromId && id > tillId) {
			const auto i = _items.find(id);
			Assert(i != end(_items));
			const auto item = i->second.get();
			ChangeItemSelection(
				selected,
				item,
				computeSelectionData(item, FullSelection));
		}
	}
}

bool Provider::allowSaveFileAs(
		not_null<const HistoryItem*> item,
		not_null<DocumentData*> document) {
	return false;
}

QString Provider::showInFolderPath(
		not_null<const HistoryItem*> item,
		not_null<DocumentData*> document) {
	return QString();
}

int64 Provider::scrollTopStatePosition(not_null<HistoryItem*> item) {
	return StoryIdFromMsgId(item->id);
}

HistoryItem *Provider::scrollTopStateItem(ListScrollTopState state) {
	if (state.item && _slice.indexOf(StoryIdFromMsgId(state.item->id))) {
		return state.item;
	} else if (const auto id = _slice.nearest(state.position)) {
		const auto full = FullMsgId(_peer->id, StoryIdToMsgId(*id));
		if (const auto item = _controller->session().data().message(full)) {
			return item;
		}
	}
	return state.item;
}

void Provider::saveState(
		not_null<Media::Memento*> memento,
		ListScrollTopState scrollState) {
	if (_aroundId != kDefaultAroundId && scrollState.item) {
		memento->setAroundId({ _peer->id, _aroundId });
		memento->setIdsLimit(_idsLimit);
		memento->setScrollTopItem(scrollState.item->globalId());
		memento->setScrollTopItemPosition(scrollState.position);
		memento->setScrollTopShift(scrollState.shift);
	}
}

void Provider::restoreState(
		not_null<Media::Memento*> memento,
		Fn<void(ListScrollTopState)> restoreScrollState) {
	if (const auto limit = memento->idsLimit()) {
		const auto wasAroundId = memento->aroundId();
		if (wasAroundId.peer == _peer->id) {
			_idsLimit = limit;
			_aroundId = StoryIdFromMsgId(wasAroundId.msg);
			restoreScrollState({
				.position = memento->scrollTopItemPosition(),
				.item = MessageByGlobalId(memento->scrollTopItem()),
				.shift = memento->scrollTopShift(),
			});
			refreshViewer();
		}
	}
}

} // namespace Info::Stories
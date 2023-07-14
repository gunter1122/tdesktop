/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "dialogs/ui/dialogs_stories_list.h"

#include "lang/lang_keys.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/popup_menu.h"
#include "ui/painter.h"
#include "styles/style_dialogs.h"

#include <QtWidgets/QApplication>

#include "base/debug_log.h"

namespace Dialogs::Stories {
namespace {

constexpr auto kSmallThumbsShown = 3;
constexpr auto kPreloadPages = 2;
constexpr auto kExpandAfterRatio = 0.72;
constexpr auto kCollapseAfterRatio = 0.68;
constexpr auto kFrictionRatio = 0.15;
constexpr auto kExpandCatchUpDuration = crl::time(200);

[[nodiscard]] int AvailableNameWidth(const style::DialogsStoriesList &st) {
	const auto &full = st.full;
	const auto &font = full.nameStyle.font;
	const auto skip = font->spacew;
	return full.photoLeft * 2 + full.photo - 2 * skip;
}

} // namespace

struct List::Layout {
	int itemsCount = 0;
	QPointF geometryShift;
	float64 expandedRatio = 0.;
	float64 ratio = 0.;
	float64 thumbnailLeft = 0.;
	float64 photoLeft = 0.;
	float64 left = 0.;
	float64 single = 0.;
	int smallSkip = 0;
	int leftFull = 0;
	int leftSmall = 0;
	int singleFull = 0;
	int singleSmall = 0;
	int startIndexSmall = 0;
	int endIndexSmall = 0;
	int startIndexFull = 0;
	int endIndexFull = 0;
};

List::List(
	not_null<QWidget*> parent,
	const style::DialogsStoriesList &st,
	rpl::producer<Content> content)
: RpWidget(parent)
, _st(st) {
	setCursor(style::cur_default);

	std::move(content) | rpl::start_with_next([=](Content &&content) {
		showContent(std::move(content));
	}, lifetime());

	setMouseTracking(true);
	resize(0, _data.empty() ? 0 : st.full.height);
}

void List::showContent(Content &&content) {
	if (_content == content) {
		return;
	}
	if (content.elements.empty()) {
		_data = {};
		_empty = true;
		return;
	}
	const auto wasCount = int(_data.items.size());
	_content = std::move(content);
	auto items = base::take(_data.items);
	_data.items.reserve(_content.elements.size());
	for (const auto &element : _content.elements) {
		const auto id = element.id;
		const auto i = ranges::find(items, id, [](const Item &item) {
			return item.element.id;
		});
		if (i != end(items)) {
			_data.items.push_back(std::move(*i));
			auto &item = _data.items.back();
			if (item.element.thumbnail != element.thumbnail) {
				item.element.thumbnail = element.thumbnail;
				item.subscribed = false;
			}
			if (item.element.name != element.name) {
				item.element.name = element.name;
				item.nameCache = QImage();
			}
			item.element.count = element.count;
			item.element.unreadCount = element.unreadCount;
		} else {
			_data.items.emplace_back(Item{ .element = element });
		}
	}
	if (int(_data.items.size()) != wasCount) {
		updateGeometry();
	}
	updateScrollMax();
	update();
	if (!wasCount) {
		_empty = false;
	}
}

void List::updateScrollMax() {
	const auto &full = _st.full;
	const auto singleFull = full.photoLeft * 2 + full.photo;
	const auto widthFull = full.left + int(_data.items.size()) * singleFull;
	_scrollLeftMax = std::max(widthFull - width(), 0);
	_scrollLeft = std::clamp(_scrollLeft, 0, _scrollLeftMax);
	checkLoadMore();
	update();
}

rpl::producer<uint64> List::clicks() const {
	return _clicks.events();
}

rpl::producer<ShowMenuRequest> List::showMenuRequests() const {
	return _showMenuRequests.events();
}

rpl::producer<bool> List::toggleExpandedRequests() const {
	return _toggleExpandedRequests.events();
}

rpl::producer<> List::entered() const {
	return _entered.events();
}

rpl::producer<> List::loadMoreRequests() const {
	return _loadMoreRequests.events();
}

void List::requestExpanded(bool expanded) {
	if (_expanded != expanded) {
		_expanded = expanded;
		const auto from = _expanded ? 0. : 1.;
		const auto till = _expanded ? 1. : 0.;
		_expandedAnimation.start([=] {
			checkForFullState();
			update();
			_collapsedGeometryChanged.fire({});
		}, from, till, st::slideWrapDuration, anim::sineInOut);
	}
	_toggleExpandedRequests.fire_copy(_expanded);
}

void List::enterEventHook(QEnterEvent *e) {
	_entered.fire({});
}

void List::resizeEvent(QResizeEvent *e) {
	updateScrollMax();
}

void List::updateExpanding(int expandingHeight, int expandedHeight) {
	Expects(!expandingHeight || expandedHeight > 0);

	const auto ratio = !expandingHeight
		? 0.
		: (float64(expandingHeight) / expandedHeight);
	if (_lastRatio == ratio) {
		return;
	}
	const auto expanding = (ratio > _lastRatio);
	_lastRatio = ratio;
	const auto change = _expanded
		? (!expanding && ratio < kCollapseAfterRatio)
		: (expanding && ratio > kExpandAfterRatio);
	if (change) {
		requestExpanded(!_expanded);
	}
}

List::Layout List::computeLayout() {
	updateExpanding(
		_lastExpandedHeight * _expandCatchUpAnimation.value(1.),
		_st.full.height);
	return computeLayout(_expandedAnimation.value(_expanded ? 1. : 0.));
}

List::Layout List::computeLayout(float64 expanded) const {
	const auto &st = _st.small;
	const auto &full = _st.full;
	const auto expandedRatio = _lastRatio;
	const auto collapsedRatio = expandedRatio * kFrictionRatio;
	const auto ratio = expandedRatio * expanded
		+ collapsedRatio * (1. - expanded);

	const auto lerp = [&](float64 a, float64 b) {
		return a + (b - a) * ratio;
	};
	const auto widthFull = width();
	const auto itemsCount = int(_data.items.size());
	const auto leftFullMin = full.left;
	const auto singleFullMin = full.photoLeft * 2 + full.photo;
	const auto totalFull = leftFullMin + singleFullMin * itemsCount;
	const auto skipSide = (totalFull < widthFull)
		? (widthFull - totalFull) / (itemsCount + 1)
		: 0;
	const auto skipBetween = (totalFull < widthFull && itemsCount > 1)
		? (widthFull - totalFull - 2 * skipSide) / (itemsCount - 1)
		: skipSide;
	const auto singleFull = singleFullMin + skipBetween;
	const auto smallSkip = (itemsCount > 1
		&& _data.items[0].element.skipSmall)
		? 1
		: 0;
	const auto smallCount = std::min(
		kSmallThumbsShown,
		itemsCount - smallSkip);
	const auto smallWidth = st.photo + (smallCount - 1) * st.shift;
	const auto leftSmall = st.left - (smallSkip ? st.shift : 0);
	const auto leftFull = full.left - _scrollLeft + skipSide;
	const auto startIndexFull = std::max(-leftFull, 0) / singleFull;
	const auto cellLeftFull = leftFull + (startIndexFull * singleFull);
	const auto endIndexFull = std::min(
		(width() - leftFull + singleFull - 1) / singleFull,
		itemsCount);
	const auto startIndexSmall = std::min(startIndexFull, smallSkip);
	const auto endIndexSmall = smallSkip + smallCount;
	const auto cellLeftSmall = leftSmall + (startIndexSmall * st.shift);
	const auto thumbnailLeftFull = cellLeftFull + full.photoLeft;
	const auto thumbnailLeftSmall = cellLeftSmall + st.photoLeft;
	const auto thumbnailLeft = lerp(thumbnailLeftSmall, thumbnailLeftFull);
	const auto photoLeft = lerp(st.photoLeft, full.photoLeft);
	return Layout{
		.itemsCount = itemsCount,
		.geometryShift = QPointF(
			(_state == State::Changing
				? (lerp(_changingGeometryFrom.x(), _geometryFull.x()) - x())
				: 0.),
			(_state == State::Changing
				? (lerp(_changingGeometryFrom.y(), _geometryFull.y()) - y())
				: 0.)),
		.expandedRatio = expandedRatio,
		.ratio = ratio,
		.thumbnailLeft = thumbnailLeft,
		.photoLeft = photoLeft,
		.left = thumbnailLeft - photoLeft,
		.single = lerp(st.shift, singleFull),
		.smallSkip = smallSkip,
		.leftFull = leftFull,
		.leftSmall = leftSmall,
		.singleFull = singleFull,
		.singleSmall = st.shift,
		.startIndexSmall = startIndexSmall,
		.endIndexSmall = endIndexSmall,
		.startIndexFull = startIndexFull,
		.endIndexFull = endIndexFull,
	};
}

void List::paintEvent(QPaintEvent *e) {
	const auto &st = _st.small;
	const auto &full = _st.full;
	const auto layout = computeLayout();
	const auto ratio = layout.ratio;
	const auto expandRatio = (ratio >= kCollapseAfterRatio)
		? 1.
		: (ratio <= kExpandAfterRatio * kFrictionRatio)
		? 0.
		: ((ratio - kExpandAfterRatio * kFrictionRatio)
			/ (kCollapseAfterRatio - kExpandAfterRatio * kFrictionRatio));
	const auto lerp = [&](float64 a, float64 b) {
		return a + (b - a) * ratio;
	};
	const auto elerp = [&](float64 a, float64 b) {
		return a + (b - a) * expandRatio;
	};
	const auto line = elerp(st.lineTwice, full.lineTwice) / 2.;
	const auto lineRead = elerp(st.lineReadTwice, full.lineReadTwice) / 2.;
	const auto photoTopSmall = st.photoTop;
	const auto photoTop = photoTopSmall
		+ (full.photoTop - photoTopSmall) * layout.expandedRatio;
	const auto photo = lerp(st.photo, full.photo);
	const auto nameScale = _lastRatio;
	const auto nameTop = full.nameTop
		+ (photoTop + photo - full.photoTop - full.photo);
	const auto nameWidth = nameScale * AvailableNameWidth(_st);
	const auto nameHeight = nameScale * full.nameStyle.font->height;
	const auto nameLeft = layout.photoLeft + (photo - nameWidth) / 2.;
	const auto readUserpicOpacity = elerp(_st.readOpacity, 1.);
	const auto readUserpicAppearingOpacity = elerp(_st.readOpacity, 0.);

	auto p = QPainter(this);

	if (_state == State::Changing) {
		p.translate(layout.geometryShift);
	}

	const auto drawSmall = (expandRatio < 1.);
	const auto drawFull = (expandRatio > 0.);
	auto hq = PainterHighQualityEnabler(p);

	const auto count = std::max(
		layout.endIndexFull - layout.startIndexFull,
		layout.endIndexSmall - layout.startIndexSmall);

	struct Single {
		float64 x = 0.;
		int indexSmall = 0;
		Item *itemSmall = nullptr;
		int indexFull = 0;
		Item *itemFull = nullptr;
		float64 photoTop = 0.;

		explicit operator bool() const {
			return itemSmall || itemFull;
		}
	};
	const auto lookup = [&](int index) {
		const auto indexSmall = layout.startIndexSmall + index;
		const auto indexFull = layout.startIndexFull + index;
		const auto ySmall = photoTopSmall
			+ ((photoTop - photoTopSmall)
				* (kSmallThumbsShown - indexSmall + layout.smallSkip) / 0.5);
		const auto y = elerp(ySmall, photoTop);

		const auto small = (drawSmall
			&& indexSmall < layout.endIndexSmall
			&& indexSmall >= layout.smallSkip)
			? &_data.items[indexSmall]
			: nullptr;
		const auto full = (drawFull && indexFull < layout.endIndexFull)
			? &_data.items[indexFull]
			: nullptr;
		const auto x = layout.left + layout.single * index;
		return Single{ x, indexSmall, small, indexFull, full, y };
	};
	const auto hasUnread = [&](const Single &single) {
		return (single.itemSmall && single.itemSmall->element.unreadCount)
			|| (single.itemFull && single.itemFull->element.unreadCount);
	};
	const auto enumerate = [&](auto &&paintGradient, auto &&paintOther) {
		auto nextGradientPainted = false;
		auto skippedPainted = false;
		const auto first = layout.smallSkip - layout.startIndexSmall;
		for (auto i = count; i != first;) {
			--i;
			const auto next = (i > 0) ? lookup(i - 1) : Single();
			const auto gradientPainted = nextGradientPainted;
			nextGradientPainted = false;
			if (const auto current = lookup(i)) {
				if (i == first && next && !skippedPainted) {
					skippedPainted = true;
					paintGradient(next);
					paintOther(next);
				}
				if (!gradientPainted) {
					paintGradient(current);
				}
				if (i > first && hasUnread(current) && next) {
					if (current.itemSmall || !next.itemSmall) {
						if (i - 1 == first
							&& first > 0
							&& !skippedPainted) {
							if (const auto skipped = lookup(i - 2)) {
								skippedPainted = true;
								paintGradient(skipped);
								paintOther(skipped);
							}
						}
						nextGradientPainted = true;
						paintGradient(next);
					}
				}
				paintOther(current);
			}
		}
	};
	enumerate([&](Single single) {
		// Name.
		if (const auto full = single.itemFull) {
			validateName(full);
			if (expandRatio > 0.) {
				p.setOpacity(expandRatio);
				p.drawImage(QRectF(
					single.x + nameLeft,
					nameTop,
					nameWidth,
					nameHeight
				), full->nameCache);
			}
		}

		// Unread gradient.
		const auto x = single.x;
		const auto userpic = QRectF(
			x + layout.photoLeft,
			single.photoTop,
			photo,
			photo);
		const auto small = single.itemSmall;
		const auto itemFull = single.itemFull;
		const auto smallUnread = small && small->element.unreadCount;
		const auto fullUnread = itemFull && itemFull->element.unreadCount;
		const auto unreadOpacity = (smallUnread && fullUnread)
			? 1.
			: smallUnread
			? (1. - expandRatio)
			: fullUnread
			? expandRatio
			: 0.;
		if (unreadOpacity > 0.) {
			p.setOpacity(unreadOpacity);
			const auto outerAdd = 2 * line;
			const auto outer = userpic.marginsAdded(
				{ outerAdd, outerAdd, outerAdd, outerAdd });
			p.setPen(Qt::NoPen);
			auto gradient = QLinearGradient(
				userpic.topRight(),
				userpic.bottomLeft());
			gradient.setStops({
				{ 0., st::groupCallLive1->c },
				{ 1., st::groupCallMuted1->c },
			});
			p.setBrush(gradient);
			p.drawEllipse(outer);
		}
		p.setOpacity(1.);
	}, [&](Single single) {
		Expects(single.itemSmall || single.itemFull);

		const auto x = single.x;
		const auto userpic = QRectF(
			x + layout.photoLeft,
			single.photoTop,
			photo,
			photo);
		const auto small = single.itemSmall;
		const auto itemFull = single.itemFull;
		const auto smallUnread = small && small->element.unreadCount;
		const auto fullUnread = itemFull && itemFull->element.unreadCount;

		// White circle with possible read gray line.
		const auto hasReadLine = (itemFull && !fullUnread);
		p.setOpacity((small && itemFull)
			? 1.
			: small
			? (1. - expandRatio)
			: expandRatio);
		if (hasReadLine) {
			auto color = st::dialogsUnreadBgMuted->c;
			if (small) {
				color.setAlphaF(color.alphaF() * expandRatio);
			}
			auto pen = QPen(color);
			pen.setWidthF(lineRead);
			p.setPen(pen);
		} else {
			p.setPen(Qt::NoPen);
		}
		const auto add = line + (hasReadLine ? (lineRead / 2.) : 0.);
		const auto rect = userpic.marginsAdded({ add, add, add, add });
		p.setBrush(st::dialogsBg);
		p.drawEllipse(rect);

		// Userpic.
		if (itemFull == small) {
			p.setOpacity(smallUnread ? 1. : readUserpicOpacity);
			validateThumbnail(itemFull);
			const auto size = full.photo;
			p.drawImage(userpic, itemFull->element.thumbnail->image(size));
		} else {
			if (small) {
				p.setOpacity(smallUnread
					? (itemFull ? 1. : (1. - expandRatio))
					: (itemFull
						? _st.readOpacity
						: readUserpicAppearingOpacity));
				validateThumbnail(small);
				const auto size = (expandRatio > 0.)
					? full.photo
					: st.photo;
				p.drawImage(userpic, small->element.thumbnail->image(size));
			}
			if (itemFull) {
				p.setOpacity(expandRatio);
				validateThumbnail(itemFull);
				const auto size = full.photo;
				p.drawImage(
					userpic,
					itemFull->element.thumbnail->image(size));
			}
		}
		p.setOpacity(1.);
	});
}

void List::validateThumbnail(not_null<Item*> item) {
	if (!item->subscribed) {
		item->subscribed = true;
		//const auto id = item.element.id;
		item->element.thumbnail->subscribeToUpdates([=] {
			update();
		});
	}
}

void List::validateName(not_null<Item*> item) {
	const auto &color = st::dialogsNameFg;
	if (!item->nameCache.isNull() && item->nameCacheColor == color->c) {
		return;
	}
	const auto &full = _st.full;
	const auto &font = full.nameStyle.font;
	const auto available = AvailableNameWidth(_st);
	const auto text = Ui::Text::String(full.nameStyle, item->element.name);
	const auto ratio = style::DevicePixelRatio();
	item->nameCacheColor = color->c;
	item->nameCache = QImage(
		QSize(available, font->height) * ratio,
		QImage::Format_ARGB32_Premultiplied);
	item->nameCache.setDevicePixelRatio(ratio);
	item->nameCache.fill(Qt::transparent);
	auto p = Painter(&item->nameCache);
	p.setPen(color);
	text.drawElided(p, 0, 0, available, 1, style::al_top);
}

void List::wheelEvent(QWheelEvent *e) {
	const auto horizontal = (e->angleDelta().x() != 0);
	if (!horizontal || _state == State::Small) {
		e->ignore();
		return;
	}
	auto delta = horizontal
		? ((style::RightToLeft() ? -1 : 1) * (e->pixelDelta().x()
			? e->pixelDelta().x()
			: e->angleDelta().x()))
		: (e->pixelDelta().y()
			? e->pixelDelta().y()
			: e->angleDelta().y());

	const auto now = _scrollLeft;
	const auto used = now - delta;
	const auto next = std::clamp(used, 0, _scrollLeftMax);
	if (next != now) {
		requestExpanded(true);
		_scrollLeft = next;
		updateSelected();
		checkLoadMore();
		update();
	}
	e->accept();
}

void List::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	} else if (_state == State::Small) {
		requestExpanded(true);
	} else if (_state != State::Full) {
		return;
	}
	_lastMousePosition = e->globalPos();
	updateSelected();

	_mouseDownPosition = _lastMousePosition;
	_pressed = _selected;
}

void List::mouseMoveEvent(QMouseEvent *e) {
	_lastMousePosition = e->globalPos();
	updateSelected();

	if (!_dragging && _mouseDownPosition && _state == State::Full) {
		if ((_lastMousePosition - *_mouseDownPosition).manhattanLength()
			>= QApplication::startDragDistance()) {
			_dragging = true;
			_startDraggingLeft = _scrollLeft;
		}
	}
	checkDragging();
}

void List::checkDragging() {
	if (_dragging) {
		const auto sign = (style::RightToLeft() ? -1 : 1);
		const auto newLeft = std::clamp(
			(sign * (_mouseDownPosition->x() - _lastMousePosition.x())
				+ _startDraggingLeft),
			0,
			_scrollLeftMax);
		if (newLeft != _scrollLeft) {
			_scrollLeft = newLeft;
			checkLoadMore();
			update();
		}
	}
}

void List::checkLoadMore() {
	if (_scrollLeftMax - _scrollLeft < width() * kPreloadPages) {
		_loadMoreRequests.fire({});
	}
}

void List::mouseReleaseEvent(QMouseEvent *e) {
	_lastMousePosition = e->globalPos();
	const auto guard = gsl::finally([&] {
		_mouseDownPosition = std::nullopt;
	});

	const auto pressed = std::exchange(_pressed, -1);
	if (finishDragging()) {
		return;
	}
	updateSelected();
	if (_selected == pressed) {
		if (!_expanded) {
			requestExpanded(true);
		} else if (_selected < _data.items.size()) {
			_clicks.fire_copy(_data.items[_selected].element.id);
		}
	}
}

void List::setExpandedHeight(int height, bool momentum) {
	height = std::clamp(height, 0, _st.full.height);
	if (_lastExpandedHeight == height) {
		return;
	} else if (momentum && _expandIgnored) {
		return;
	} else if (momentum && height > 0 && !_lastExpandedHeight) {
		_expandIgnored = true;
		return;
	} else if (!momentum && _expandIgnored && height > 0) {
		_expandIgnored = false;
		_expandCatchUpAnimation.start([=] {
			update();
			checkForFullState();
		}, 0., 1., kExpandCatchUpDuration);
	} else if (!height && _expandCatchUpAnimation.animating()) {
		_expandCatchUpAnimation.stop();
	}
	_lastExpandedHeight = height;
	if (!checkForFullState()) {
		setState(!height ? State::Small : State::Changing);
	}
	update();
}

bool List::checkForFullState() {
	if (_expandCatchUpAnimation.animating()
		|| _expandedAnimation.animating()
		|| _lastExpandedHeight < _st.full.height) {
		return false;
	}
	setState(State::Full);
	return true;
}

void List::setLayoutConstraints(
		QPoint positionSmall,
		style::align alignSmall,
		QRect geometryFull) {
	_positionSmall = positionSmall;
	_alignSmall = alignSmall;
	_geometryFull = geometryFull;
	updateGeometry();
	update();
}

List::CollapsedGeometry List::collapsedGeometryCurrent() const {
	const auto expanded = _expandedAnimation.value(_expanded ? 1. : 0.);
	if (expanded == 1.) {
		return { QRect(), 1. };
	}
	const auto layout = computeLayout(0.);
	const auto small = countSmallGeometry();
	const auto index = layout.smallSkip - layout.startIndexSmall;
	const auto shift = x() + layout.geometryShift.x();
	const auto left = int(base::SafeRound(
		shift + layout.left + layout.single * index));
	const auto width = small.x() + small.width() - left;
	return {
		QRect(left, small.y(), width, small.height()),
		expanded,
	};
}

rpl::producer<> List::collapsedGeometryChanged() const {
	return _collapsedGeometryChanged.events();
}

void List::updateGeometry() {
	switch (_state) {
	case State::Small: setGeometry(countSmallGeometry()); break;
	case State::Changing: {
		_changingGeometryFrom = countSmallGeometry();
		setGeometry(_geometryFull.united(_changingGeometryFrom));
	} break;
	case State::Full: setGeometry(_geometryFull);
	}
	update();
}

QRect List::countSmallGeometry() const {
	const auto &st = _st.small;
	const auto layout = computeLayout(0.);
	const auto count = layout.endIndexSmall
		- std::max(layout.startIndexSmall, layout.smallSkip);
	const auto width = st.left
		+ st.photoLeft
		+ st.photo + (count - 1) * st.shift
		+ st.photoLeft
		+ st.left;
	const auto left = ((_alignSmall & Qt::AlignRight) == Qt::AlignRight)
		? (_positionSmall.x() - width)
		: ((_alignSmall & Qt::AlignCenter) == Qt::AlignCenter)
		? (_positionSmall.x() - (width / 2))
		: _positionSmall.x();
	return QRect(
		left,
		_positionSmall.y(),
		width,
		st.photoTop + st.photo + st.photoTop);
}

void List::setState(State state) {
	if (_state == state) {
		return;
	}
	_state = state;
	updateGeometry();
}

void List::contextMenuEvent(QContextMenuEvent *e) {
	_menu = nullptr;

	if (e->reason() == QContextMenuEvent::Mouse) {
		_lastMousePosition = e->globalPos();
		updateSelected();
	}
	if (_selected < 0 || _data.empty() || !_expanded) {
		return;
	}
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	_showMenuRequests.fire({
		_data.items[_selected].element.id,
		Ui::Menu::CreateAddActionCallback(_menu),
	});
	if (_menu->empty()) {
		_menu = nullptr;
		return;
	}
	const auto updateAfterMenuDestroyed = [=] {
		const auto globalPosition = QCursor::pos();
		if (rect().contains(mapFromGlobal(globalPosition))) {
			_lastMousePosition = globalPosition;
			updateSelected();
		}
	};
	QObject::connect(
		_menu.get(),
		&QObject::destroyed,
		crl::guard(&_menuGuard, updateAfterMenuDestroyed));
	_menu->popup(e->globalPos());
	e->accept();
}

bool List::finishDragging() {
	if (!_dragging) {
		return false;
	}
	checkDragging();
	_dragging = false;
	updateSelected();
	return true;
}

void List::updateSelected() {
	if (_pressed >= 0) {
		return;
	}
	const auto &st = _st.small;
	const auto p = mapFromGlobal(_lastMousePosition);
	const auto layout = computeLayout();
	const auto firstRightFull = layout.leftFull
		+ (layout.startIndexFull + 1) * layout.singleFull;
	const auto secondLeftFull = firstRightFull;
	const auto firstRightSmall = layout.leftSmall
		+ st.photoLeft
		+ st.photo;
	const auto secondLeftSmall = layout.smallSkip
		? (layout.leftSmall + st.photoLeft + st.shift)
		: firstRightSmall;
	const auto lastRightAddFull = 0;
	const auto lastRightAddSmall = st.photoLeft;
	const auto lerp = [&](float64 a, float64 b) {
		return a + (b - a) * layout.ratio;
	};
	const auto firstRight = lerp(firstRightSmall, firstRightFull);
	const auto secondLeft = lerp(secondLeftSmall, secondLeftFull);
	const auto lastRightAdd = lerp(lastRightAddSmall, lastRightAddFull);
	const auto activateFull = (layout.ratio >= 0.5);
	const auto startIndex = activateFull
		? layout.startIndexFull
		: layout.startIndexSmall;
	const auto endIndex = activateFull
		? layout.endIndexFull
		: layout.endIndexSmall;
	const auto x = p.x();
	const auto infiniteIndex = (x < secondLeft)
		? 0
		: int(
			std::floor((std::max(x - firstRight, 0.)) / layout.single) + 1);
	const auto index = (endIndex == startIndex)
		? -1
		: (infiniteIndex == endIndex - startIndex
			&& x < firstRight
				+ (endIndex - startIndex - 1) * layout.single
				+ lastRightAdd)
		? (infiniteIndex - 1) // Last small part should still be clickable.
		: (startIndex + infiniteIndex >= endIndex)
		? (_st.fullClickable ? (endIndex - 1) : -1)
		: infiniteIndex;
	const auto selected = (index < 0
		|| startIndex + index >= layout.itemsCount)
		? -1
		: (startIndex + index);
	if (_selected != selected) {
		const auto over = (selected >= 0);
		if (over != (_selected >= 0)) {
			setCursor(over ? style::cur_pointer : style::cur_default);
		}
		_selected = selected;
	}
}

} // namespace Dialogs::Stories
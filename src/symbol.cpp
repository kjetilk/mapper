/*
 *    Copyright 2012, 2013 Thomas Schöps
 *    Copyright 2012-2015 Kai Pastor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "symbol.h"

#include <QDebug>
#include <QFile>
#include <QPainter>
#include <QStringBuilder>
#include <QTextDocument>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <qmath.h>

#include "core/map_color.h"
#include "core/map_view.h"
#include "file_import_export.h"
#include "map.h"
#include "object.h"
#include "object_text.h"
#include "renderable_implementation.h"
#include "symbol_area.h"
#include "symbol_combined.h"
#include "symbol_line.h"
#include "symbol_point.h"
#include "symbol_properties_widget.h"
#include "symbol_setting_dialog.h"
#include "symbol_text.h"
#include "util.h"
#include "settings.h"

Symbol::Symbol(Type type)
 : type(type)
 , is_helper_symbol(false)
 , is_hidden(false)
 , is_protected(false)
{
	for (int i = 0; i < number_components; ++i)
		number[i] = -1;
}

Symbol::~Symbol()
{
	; // nothing
}

bool Symbol::equals(const Symbol* other, Qt::CaseSensitivity case_sensitivity, bool compare_state) const
{
	if (type != other->type)
		return false;
	for (int i = 0; i < number_components; ++i)
	{
		if (number[i] != other->number[i])
			return false;
		if (number[i] == -1 && other->number[i] == -1)
			break;
	}
	if (is_helper_symbol != other->is_helper_symbol)
		return false;
	
	if (compare_state && (is_hidden != other->is_hidden || is_protected != other->is_protected))
		return false;
	
	if (name.compare(other->name, case_sensitivity) != 0)
		return false;
	if (description.compare(other->description, case_sensitivity) != 0)
		return false;
	
	return equalsImpl(other, case_sensitivity);
}

const PointSymbol* Symbol::asPoint() const
{
	Q_ASSERT(type == Point);
	return static_cast<const PointSymbol*>(this);
}
PointSymbol* Symbol::asPoint()
{
	Q_ASSERT(type == Point);
	return static_cast<PointSymbol*>(this);
}
const LineSymbol* Symbol::asLine() const
{
	Q_ASSERT(type == Line);
	return static_cast<const LineSymbol*>(this);
}
LineSymbol* Symbol::asLine()
{
	Q_ASSERT(type == Line);
	return static_cast<LineSymbol*>(this);
}
const AreaSymbol* Symbol::asArea() const
{
	Q_ASSERT(type == Area);
	return static_cast<const AreaSymbol*>(this);
}
AreaSymbol* Symbol::asArea()
{
	Q_ASSERT(type == Area);
	return static_cast<AreaSymbol*>(this);
}
const TextSymbol* Symbol::asText() const
{
	Q_ASSERT(type == Text);
	return static_cast<const TextSymbol*>(this);
}
TextSymbol* Symbol::asText()
{
	Q_ASSERT(type == Text);
	return static_cast<TextSymbol*>(this);
}
const CombinedSymbol* Symbol::asCombined() const
{
	Q_ASSERT(type == Combined);
	return static_cast<const CombinedSymbol*>(this);
}
CombinedSymbol* Symbol::asCombined()
{
	Q_ASSERT(type == Combined);
	return static_cast<CombinedSymbol*>(this);
}

bool Symbol::isTypeCompatibleTo(const Object* object) const
{
	if (type == Point && object->getType() == Object::Point)
		return true;
	else if ((type == Line || type == Area || type == Combined) && object->getType() == Object::Path)
		return true;
	else if (type == Text && object->getType() == Object::Text)
		return true;

	return false;
}

bool Symbol::numberEquals(const Symbol* other, bool ignore_trailing_zeros)
{
	if (ignore_trailing_zeros)
	{
		for (int i = 0; i < number_components; ++i)
		{
			if (number[i] == -1 && other->number[i] == -1)
				return true;
			if ((number[i] == 0 || number[i] == -1) &&
				(other->number[i] == 0 || other->number[i] == -1))
				continue;
			if (number[i] != other->number[i])
				return false;
		}
	}
	else
	{
		for (int i = 0; i < number_components; ++i)
		{
			if (number[i] != other->number[i])
				return false;
			if (number[i] == -1)
				return true;
		}
	}
	return true;
}

#ifndef NO_NATIVE_FILE_FORMAT

bool Symbol::load(QIODevice* file, int version, Map* map)
{
	loadString(file, name);
	for (int i = 0; i < number_components; ++i)
		file->read((char*)&number[i], sizeof(int));
	loadString(file, description);
	file->read((char*)&is_helper_symbol, sizeof(bool));
	if (version >= 10)
		file->read((char*)&is_hidden, sizeof(bool));
	if (version >= 11)
		file->read((char*)&is_protected, sizeof(bool));
	
	return loadImpl(file, version, map);
}

#endif

void Symbol::save(QXmlStreamWriter& xml, const Map& map) const
{
	xml.writeStartElement("symbol");
	xml.writeAttribute("type", QString::number(type));
	int id = map.findSymbolIndex(this);
	if (id >= 0)
		xml.writeAttribute("id", QString::number(id)); // unique if given
	xml.writeAttribute("code", getNumberAsString()); // not always unique
	if (!name.isEmpty())
		xml.writeAttribute("name", name);
	if (is_helper_symbol)
		xml.writeAttribute("is_helper_symbol","true");
	if (is_hidden)
		xml.writeAttribute("is_hidden","true");
	if (is_protected)
		xml.writeAttribute("is_protected","true");
	if (!description.isEmpty())
		xml.writeTextElement("description", description);
	saveImpl(xml, map);
	xml.writeEndElement(/*symbol*/);
}

Symbol* Symbol::load(QXmlStreamReader& xml, const Map& map, SymbolDictionary& symbol_dict)
{
	Q_ASSERT(xml.name() == "symbol");
	
	int symbol_type = xml.attributes().value("type").toString().toInt();
	Symbol* symbol = Symbol::getSymbolForType(static_cast<Symbol::Type>(symbol_type));
	if (!symbol)
		throw FileFormatException(ImportExport::tr("Error while loading a symbol of type %1 at line %2 column %3.").arg(symbol_type).arg(xml.lineNumber()).arg(xml.columnNumber()));
	
	QXmlStreamAttributes attributes = xml.attributes();
	QString code = attributes.value("code").toString();
	if (attributes.hasAttribute("id"))
	{
		QString id = attributes.value("id").toString();
		if (symbol_dict.contains(id)) 
			throw FileFormatException(ImportExport::tr("Symbol ID '%1' not unique at line %2 column %3.").arg(id).arg(xml.lineNumber()).arg(xml.columnNumber()));
		
		symbol_dict[id] = symbol;
		
		if (code.isEmpty())
			code = id;
	}
	
	if (code.isEmpty())
		symbol->number[0] = -1;
	else
	{
		for (int i = 0, index = 0; i < number_components && index >= 0; ++i)
		{
			if (index == -1)
				symbol->number[i] = -1;
			else
			{
				int dot = code.indexOf(".", index+1);
				int num = code.mid(index, (dot == -1) ? -1 : (dot - index)).toInt();
				symbol->number[i] = num;
				index = dot;
				if (index != -1)
					index++;
			}
		}
	}
	
	symbol->name = attributes.value("name").toString();
	symbol->is_helper_symbol = (attributes.value("is_helper_symbol") == "true");
	symbol->is_hidden = (attributes.value("is_hidden") == "true");
	symbol->is_protected = (attributes.value("is_protected") == "true");
	
	while (xml.readNextStartElement())
	{
		if (xml.name() == "description")
			symbol->description = xml.readElementText();
		else
		{
			if (!symbol->loadImpl(xml, map, symbol_dict))
				xml.skipCurrentElement();
		}
	}
	
	if (xml.error())
	{
		delete symbol;
		throw FileFormatException(
		            ImportExport::tr("Error while loading a symbol of type %1 at line %2 column %3: %4")
		            .arg(symbol_type)
		            .arg(xml.lineNumber())
		            .arg(xml.columnNumber())
		            .arg(xml.errorString()) );
	}
	
	return symbol;
}

bool Symbol::loadFinished(Map* map)
{
	Q_UNUSED(map);
	return true;
}

void Symbol::createRenderables(const PathObject*, const PathPartVector&, ObjectRenderables&, Symbol::RenderableOptions) const
{
	qWarning("Missing implementation of Symbol::createRenderables(const PathObject*, const PathPartVector&, ObjectRenderables&)");
}

void Symbol::createBaselineRenderables(
        const PathObject*,
        const PathPartVector& path_parts,
        ObjectRenderables& output,
        const MapColor* color) const
{
	Q_ASSERT((getContainedTypes() & ~(Symbol::Line | Symbol::Area | Symbol::Combined)) == 0);
	
	if (color)
	{
		// Insert line renderable
		LineSymbol line_symbol;
		line_symbol.setColor(color);
		line_symbol.setLineWidth(0);
		for (const auto& part : path_parts)
		{
			auto line_renderable = new LineRenderable(&line_symbol, part, false);
			output.insertRenderable(line_renderable);
		}
	}
}

bool Symbol::symbolChanged(const Symbol* old_symbol, const Symbol* new_symbol)
{
	Q_UNUSED(old_symbol);
	Q_UNUSED(new_symbol);
	return false;
}

bool Symbol::containsSymbol(const Symbol* symbol) const
{
	Q_UNUSED(symbol);
	return false;
}

QImage Symbol::getIcon(const Map* map, bool update) const
{
	if (update || icon.isNull())
		icon = createIcon(map, Settings::getInstance().getSymbolWidgetIconSizePx(), true, 1);
	
	return icon;
}

QImage Symbol::createIcon(const Map* map, int side_length, bool antialiasing, int bottom_right_border, float best_zoom) const
{
	Type contained_types = getContainedTypes();
	
	// Create icon map and view
	Map icon_map;
	// const_cast promise: We won't change the colors, thus we won't change map.
	icon_map.useColorsFrom(const_cast<Map*>(map));
	icon_map.setScaleDenominator(map->getScaleDenominator());
	MapView view(&icon_map);
	
	// If the icon is bigger than the rectangle with this zoom factor, it is zoomed out to fit into the rectangle
	view.setZoom(best_zoom);
	int white_border_pixels = 0;
	if (contained_types & Line || contained_types & Area || type == Combined)
		white_border_pixels = 0;
	else if (contained_types & Point || contained_types & Text)
		white_border_pixels = 2;
	else
		Q_ASSERT(false);
	const float max_icon_mm = 0.001f * view.pixelToLength(side_length - bottom_right_border - white_border_pixels);
	const float max_icon_mm_half = 0.5f * max_icon_mm;
	
	// Create image
	QImage image(side_length, side_length, QImage::Format_ARGB32_Premultiplied);
	QPainter painter;
	painter.begin(&image);
	if (antialiasing)
		painter.setRenderHint(QPainter::Antialiasing);
	
	// Make background transparent
	QPainter::CompositionMode mode = painter.compositionMode();
	painter.setCompositionMode(QPainter::CompositionMode_Clear);
	painter.fillRect(image.rect(), Qt::transparent);
	painter.setCompositionMode(mode);
	
	// Create geometry
	Object* object = NULL;
	Symbol* icon_symbol = NULL;
	if (type == Point)
	{
		PointObject* point = new PointObject(this);
		point->setPosition(0, 0);
		object = point;
	}
	else if (type == Area || (type == Combined && getContainedTypes() & Area))
	{
		PathObject* path = new PathObject(this);
		path->addCoordinate(0, MapCoord(-max_icon_mm_half, -max_icon_mm_half));
		path->addCoordinate(1, MapCoord(max_icon_mm_half, -max_icon_mm_half));
		path->addCoordinate(2, MapCoord(max_icon_mm_half, max_icon_mm_half));
		path->addCoordinate(3, MapCoord(-max_icon_mm_half, max_icon_mm_half));
		path->parts().front().setClosed(true, true);
		object = path;
	}
	else if (type == Line || type == Combined)
	{
		const Symbol* symbol_to_use = this;
		bool show_dash_symbol = false;
		if (type == Line)
		{
			// If there are breaks in the line, scale them down so they fit into the icon exactly
			// TODO: does not work for combined lines yet. Could be done by checking every contained line and scaling the painter horizontally
			const LineSymbol* line = asLine();
			if (line->isDashed() && line->getBreakLength() > 0)
			{
				LineSymbol* icon_line = duplicate()->asLine();
				
				float ideal_length = 0.001 * (2 * line->getDashesInGroup() * line->getDashLength() + 2 * (line->getDashesInGroup() - 1) * line->getInGroupBreakLength() + line->getBreakLength());
				float real_length = max_icon_mm;
				float factor = qMin(1.0f, real_length / qMax(0.001f, ideal_length));
				
				icon_line->setDashLength(factor * icon_line->getDashLength());
				icon_line->setBreakLength(factor * icon_line->getBreakLength());
				icon_line->setInGroupBreakLength(factor * icon_line->getInGroupBreakLength());
				
				icon_symbol = icon_line;
				symbol_to_use = icon_symbol;
			}
			else if (line->getDashSymbol() != NULL)
			{
				show_dash_symbol = !line->getDashSymbol()->isEmpty();
			}
		}
		
		PathObject* path = new PathObject(symbol_to_use);
		path->addCoordinate(0, MapCoord(-max_icon_mm_half, 0.0));
		path->addCoordinate(1, MapCoord(max_icon_mm_half, 0.0));
		if (show_dash_symbol)
		{
			MapCoord dash_coord(0, 0);
			dash_coord.setDashPoint(true);
			path->addCoordinate(1, dash_coord);
		}
		object = path;
	}
	else if (type == Text)
	{
		TextObject* text = new TextObject(this);
		text->setAnchorPosition(0, 0);
		text->setHorizontalAlignment(TextObject::AlignHCenter);
		text->setVerticalAlignment(TextObject::AlignVCenter);
		text->setText(dynamic_cast<const TextSymbol*>(this)->getIconText());
		object = text;
	}
	/*else if (type == Combined)
	{
		PathObject* path = new PathObject(this);
		for (int i = 0; i < 5; ++i)
			path->addCoordinate(i, MapCoord(sin(2*M_PI * i/5.0) * max_icon_mm_half, -cos(2*M_PI * i/5.0) * max_icon_mm_half));
		path->parts().front().setClosed(true, false);
		object = path;
	}*/
	else
		Q_ASSERT(false);
	
	icon_map.addObject(object);
	
	qreal real_icon_mm_half;
	if (type == Point || type == Text)
	{
		// Center on the object's extent center
		real_icon_mm_half = qMax(object->getExtent().width() / 2.0, object->getExtent().height() / 2.0);
		view.setCenter(MapCoord{ object->getExtent().center() });
	}
	else if (contained_types & Line && !(contained_types & Area))
	{
		// Center horizontally on extent
		real_icon_mm_half = qMax((qreal)(object->getExtent().width() / 2.0), object->getExtent().bottom());
		real_icon_mm_half = qMax(real_icon_mm_half, -object->getExtent().top());
		auto pos = MapCoord{ object->getExtent().center() };
		pos.setY(0);
		view.setCenter(pos);
	}
	else
	{
		// Center on coordinate system origin
		real_icon_mm_half = qMax(object->getExtent().right(), object->getExtent().bottom());
		real_icon_mm_half = qMax(real_icon_mm_half, -object->getExtent().left());
		real_icon_mm_half = qMax(real_icon_mm_half, -object->getExtent().top());
	}
	if (real_icon_mm_half > max_icon_mm_half)
		view.setZoom(best_zoom * (max_icon_mm_half / real_icon_mm_half));
	
	painter.translate(0.5f * (side_length - bottom_right_border), 0.5f * (side_length - bottom_right_border));
	painter.setWorldTransform(view.worldTransform(), true);
	
	RenderConfig config = { *map, QRectF(-10000, -10000, 20000, 20000), view.calculateFinalZoomFactor(), RenderConfig::HelperSymbols, 1.0 };
	bool was_hidden = is_hidden;
	is_hidden = false; // ensure that an icon is created for hidden symbols.
	icon_map.draw(&painter, config);
	is_hidden = was_hidden;
	
	delete icon_symbol;
	painter.end();
	
	return image;
}

float Symbol::calculateLargestLineExtent(Map* map) const
{
	Q_UNUSED(map);
	return 0.0f;
}

QString Symbol::getPlainTextName() const
{
	if (name.contains('<'))
	{
		QTextDocument doc;
		doc.setHtml(name);
		return doc.toPlainText();
	}
	else
		return name;
}

QString Symbol::getNumberAsString() const
{
	QString str = "";
	for (int i = 0; i < number_components; ++i)
	{
		if (number[i] < 0)
			break;
		if (!str.isEmpty())
			str += ".";
		str += QString::number(number[i]);
	}
	return str;
}

Symbol* Symbol::getSymbolForType(Symbol::Type type)
{
	if (type == Symbol::Point)
		return new PointSymbol();
	else if (type == Symbol::Line)
		return new LineSymbol();
	else if (type == Symbol::Area)
		return new AreaSymbol();
	else if (type == Symbol::Text)
		return new TextSymbol();
	else if (type == Symbol::Combined)
		return new CombinedSymbol();
	else
	{
		Q_ASSERT(false);
		return NULL;
	}
}

#ifndef NO_NATIVE_FILE_FORMAT

bool Symbol::loadSymbol(Symbol*& symbol, QIODevice* stream, int version, Map* map)
{
	int save_type;
	stream->read((char*)&save_type, sizeof(int));
	symbol = Symbol::getSymbolForType(static_cast<Symbol::Type>(save_type));
	if (!symbol)
		return false;
	if (!symbol->load(stream, version, map))
		return false;
	return true;
}

#endif

bool Symbol::areTypesCompatible(Symbol::Type a, Symbol::Type b)
{
	return (getCompatibleTypes(a) & b) != 0;
}

int Symbol::getCompatibleTypes(Symbol::Type type)
{
	if (type == Point)
		return Point;
	else if (type == Line || type == Area || type == Combined)
		return Line | Area | Combined;
	else if (type == Text)
		return Text;
	
	Q_ASSERT(false);
	return type;
}

void Symbol::duplicateImplCommon(const Symbol* other)
{
	type = other->type;
	name = other->name;
	for (int i = 0; i < number_components; ++i)
		number[i] = other->number[i];
	description = other->description;
	is_helper_symbol = other->is_helper_symbol;
	is_hidden = other->is_hidden;
	icon = other->icon;
}

SymbolPropertiesWidget* Symbol::createPropertiesWidget(SymbolSettingDialog* dialog)
{
	return new SymbolPropertiesWidget(this, dialog);
}


bool Symbol::compareByNumber(const Symbol* s1, const Symbol* s2)
{
	int n1 = s1->number_components, n2 = s2->number_components;
	for (int i = 0; i < n1 && i < n2; i++)
	{
		if (s1->getNumberComponent(i) < s2->getNumberComponent(i)) return true;  // s1 < s2
		if (s1->getNumberComponent(i) > s2->getNumberComponent(i)) return false; // s1 > s2
		// if s1 == s2, loop to the next component
	}
	return false; // s1 == s2
}

bool Symbol::compareByColorPriority(const Symbol* s1, const Symbol* s2)
{
	const MapColor* c1 = s1->guessDominantColor();
	const MapColor* c2 = s2->guessDominantColor();
	
	if (c1 && c2)
		return c1->comparePriority(*c2);
	else if (c2)
		return true;
	else if (c1)
		return false;
	
	return false; // s1 == s2
}

Symbol::compareByColor::compareByColor(Map* map)
{
	int next_priority = map->getNumColors() - 1;
	// Iterating in reverse order so identical colors are at the position where they appear with lowest priority.
	for (int i = map->getNumColors() - 1; i >= 0; --i)
	{
		QRgb color_code = QRgb(*map->getColor(i));
		if (!color_map.contains(color_code))
		{
			color_map.insert(color_code, next_priority);
			--next_priority;
		}
	}
}

bool Symbol::compareByColor::operator() (const Symbol* s1, const Symbol* s2)
{
	const MapColor* c1 = s1->guessDominantColor();
	const MapColor* c2 = s2->guessDominantColor();
	
	if (c1 && c2)
		return color_map.value(QRgb(*c1)) < color_map.value(QRgb(*c2));
	else if (c2)
		return true;
	else if (c1)
		return false;
	
	return false; // s1 == s2
}

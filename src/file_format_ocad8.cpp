/*
 *    Copyright 2012 Pete Curtis
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

#include "file_format_ocad8.h"

#include <QDebug>
#include <QDateTime>
#include <qmath.h>
#include <QImageReader>

#include "map_color.h"
#include "object.h"
#include "symbol.h"
#include "symbol_point.h"
#include "symbol_line.h"
#include "symbol_area.h"
#include "symbol_text.h"
#include "symbol_combined.h"
#include "template_image.h"
#include "object_text.h"
#include "util.h"

#if (QT_VERSION < QT_VERSION_CHECK(4, 7, 0))
#define currentMSecsSinceEpoch() currentDateTime().toTime_t() * 1000
#endif

bool OCAD8FileFormat::understands(const unsigned char *buffer, size_t sz) const
{
    // The first two bytes of the file must be AD 0C.
    if (sz >= 2 && buffer[0] == 0xAD && buffer[1] == 0x0C) return true;
    return false;
}

Importer *OCAD8FileFormat::createImporter(QIODevice* stream, const QString &path, Map *map, MapView *view) const throw (FormatException)
{
	return new OCAD8FileImport(stream, path, map, view);
}

Exporter* OCAD8FileFormat::createExporter(QIODevice* stream, const QString& path, Map* map, MapView* view) const throw (FormatException)
{
    return new OCAD8FileExport(stream, map, view);
}




OCAD8FileImport::OCAD8FileImport(QIODevice* stream, const QString &path, Map* map, MapView* view) : Importer(stream, map, view), path(path), file(NULL)
{
    ocad_init();
    encoding_1byte = QTextCodec::codecForName("Windows-1252");
    encoding_2byte = QTextCodec::codecForName("UTF-16LE");
    offset_x = offset_y = 0;
}

OCAD8FileImport::~OCAD8FileImport()
{
    ocad_shutdown();
}

void OCAD8FileImport::doImport(bool load_symbols_only) throw (FormatException)
{
	stream->close();	// TODO: use stream instead of libocad's direct file access
    //qint64 start = QDateTime::currentMSecsSinceEpoch();

	QByteArray filename = path.toLocal8Bit().constData();
    int err = ocad_file_open(&file, filename);
    //qDebug() << "open ocad file" << err;
    if (err != 0) throw FormatException(QObject::tr("Could not open file: libocad returned %1").arg(err));
	
	if (file->header->major <= 5 || file->header->major >= 9)
		throw FormatException(QObject::tr("OCAD files of version %1 cannot be loaded!").arg(file->header->major));

    //qDebug() << "file version is" << file->header->major << ", type is"
    //         << ((file->header->ftype == 2) ? "normal" : "other");
    //qDebug() << "map scale is" << file->setup->scale;

    map->setScaleDenominator(file->setup->scale);

	map->setMapNotes(convertCString((const char*)file->buffer + file->header->infopos, file->header->infosize, false));

    // TODO: GPS projection parameters

    // TODO: print parameters

    // Load colors
    int num_colors = ocad_color_count(file);

    for (int i = 0; i < num_colors; i++)
    {
        OCADColor *ocad_color = ocad_color_at(file, i);

        MapColor* color = new MapColor();
        color->priority = i;

        // OCAD stores CMYK values as integers from 0-200.
        color->c = 0.005f * ocad_color->cyan;
        color->m = 0.005f * ocad_color->magenta;
        color->y = 0.005f * ocad_color->yellow;
        color->k = 0.005f * ocad_color->black;
        color->opacity = 1.0f;
		color->name = convertPascalString(ocad_color->name);
        color->updateFromCMYK();

        map->color_set->colors.push_back(color);
        color_index[ocad_color->number] = color;
    }

    // Load symbols
    for (OCADSymbolIndex *idx = ocad_symidx_first(file); idx != NULL; idx = ocad_symidx_next(file, idx))
    {
        for (int i = 0; i < 256; i++)
        {
            OCADSymbol *ocad_symbol = ocad_symbol_at(file, idx, i);
            if (ocad_symbol != NULL && ocad_symbol->number != 0)
            {
                Symbol *symbol = NULL;
                if (ocad_symbol->type == OCAD_POINT_SYMBOL)
                {
                    symbol = importPointSymbol((OCADPointSymbol *)ocad_symbol);
                }
                else if (ocad_symbol->type == OCAD_LINE_SYMBOL)
                {
                    symbol = importLineSymbol((OCADLineSymbol *)ocad_symbol);
                }
                else if (ocad_symbol->type == OCAD_AREA_SYMBOL)
                {
                    symbol = importAreaSymbol((OCADAreaSymbol *)ocad_symbol);
                }
                else if (ocad_symbol->type == OCAD_TEXT_SYMBOL)
                {
                    symbol = importTextSymbol((OCADTextSymbol *)ocad_symbol);
                }
                else if (ocad_symbol->type == OCAD_RECT_SYMBOL)
                {
					RectangleInfo* rect = importRectSymbol((OCADRectSymbol *)ocad_symbol);
					map->symbols.push_back(rect->border_line);
					if (rect->has_grid)
					{
						map->symbols.push_back(rect->inner_line);
						map->symbols.push_back(rect->text);
					}
					continue;
                }
                

                if (symbol)
                {
                    map->symbols.push_back(symbol);
                    symbol_index[ocad_symbol->number] = symbol;
					
					// For combined symbols, also add their parts
					// FIXME: implement private parts for combined symbols instead
					if (symbol->getType() == Symbol::Combined)
					{
						CombinedSymbol* combined_symbol = reinterpret_cast<CombinedSymbol*>(symbol);
						for (int i = 0; i < combined_symbol->getNumParts(); ++i)
						{
							Symbol* part = combined_symbol->getPart(i);
							part->setNumberComponent(2, i+1);
							map->symbols.push_back(part);
						}
					}
                }
                else
                {
                    addWarning(QObject::tr("Unable to import symbol \"%3\" (%1.%2)")
                               .arg(ocad_symbol->number / 10).arg(ocad_symbol->number % 10)
                               .arg(convertPascalString(ocad_symbol->name)));
                }
            }
        }
    }

    if (!load_symbols_only)
	{
		// Load objects

		// Place all objects into a single OCAD import layer
		MapLayer* layer = new MapLayer(QObject::tr("OCAD import layer"), map);
		for (OCADObjectIndex *idx = ocad_objidx_first(file); idx != NULL; idx = ocad_objidx_next(file, idx))
		{
			for (int i = 0; i < 256; i++)
			{
				OCADObjectEntry *entry = ocad_object_entry_at(file, idx, i);
				OCADObject *ocad_obj = ocad_object(file, entry);
				if (ocad_obj != NULL)
				{
					Object *object = importObject(ocad_obj, layer);
					if (object != NULL) {
						layer->objects.push_back(object);
					}
				}
			}
		}
		delete map->layers[0];
		map->layers[0] = layer;
		map->current_layer_index = 0;

		// Load templates
		map->templates.clear();
		for (OCADStringIndex *idx = ocad_string_index_first(file); idx != NULL; idx = ocad_string_index_next(file, idx))
		{
			for (int i = 0; i < 256; i++)
			{
				OCADStringEntry *entry = ocad_string_entry_at(file, idx, i);
				if (entry->type != 0 && entry->size > 0)
					importString(entry);
			}
		}
		map->first_front_template = map->templates.size(); // Templates in front of the map are not supported by OCD

		// Fill view with relevant fields from OCD file
		if (view)
		{
			if (file->setup->zoom >= MapView::zoom_out_limit && file->setup->zoom <= MapView::zoom_in_limit)
				view->setZoom(file->setup->zoom);
			
			s32 buf[3];
			ocad_point(buf, &file->setup->center);
			MapCoord center_pos;
			convertPoint(center_pos, buf[0], buf[1]);
			view->setPositionX(center_pos.rawX());
			view->setPositionY(center_pos.rawY());
		}
		
		// TODO: read template visibilities
		/*
			int num_template_visibilities;
			file->read((char*)&num_template_visibilities, sizeof(int));

			for (int i = 0; i < num_template_visibilities; ++i)
			{
				int pos;
				file->read((char*)&pos, sizeof(int));

                TemplateVisibility* vis = getTemplateVisibility(map->getTemplate(pos));
				file->read((char*)&vis->visible, sizeof(bool));
				file->read((char*)&vis->opacity, sizeof(float));
			}
		}
		*/

		// Undo steps are not supported in OCAD
    }

    ocad_file_close(file);

    //qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - start;
	//qDebug() << "OCAD map imported:"<<map->getNumSymbols()<<"symbols and"<<map->getNumObjects()<<"objects in"<<elapsed<<"milliseconds";
}

void OCAD8FileImport::setStringEncodings(const char *narrow, const char *wide) {
    encoding_1byte = QTextCodec::codecForName(narrow);
    encoding_2byte = QTextCodec::codecForName(wide);
}

Symbol *OCAD8FileImport::importPointSymbol(const OCADPointSymbol *ocad_symbol)
{
    PointSymbol *symbol = importPattern(ocad_symbol->ngrp, (OCADPoint *)ocad_symbol->pts);
    fillCommonSymbolFields(symbol, (OCADSymbol *)ocad_symbol);
	symbol->setRotatable(ocad_symbol->base_flags & 1);

    return symbol;
}

Symbol *OCAD8FileImport::importLineSymbol(const OCADLineSymbol *ocad_symbol)
{
	// Import a main line?
	LineSymbol *main_line = NULL;
	if (ocad_symbol->dmode == 0 || ocad_symbol->width > 0)
	{
		main_line = new LineSymbol();
		fillCommonSymbolFields(main_line, (OCADSymbol *)ocad_symbol);

		main_line->minimum_length = 0; // OCAD 8 does not store min length

		// Basic line options
		main_line->line_width = convertSize(ocad_symbol->width);
		main_line->color = convertColor(ocad_symbol->color);
		
		// Cap and join styles
		if (ocad_symbol->ends == 0)
		{
			main_line->cap_style = LineSymbol::FlatCap;
			main_line->join_style = LineSymbol::BevelJoin;
		}
		else if (ocad_symbol->ends == 1)
		{
			main_line->cap_style = LineSymbol::RoundCap;
			main_line->join_style = LineSymbol::RoundJoin;
		}
		else if (ocad_symbol->ends == 2)
		{
			main_line->cap_style = LineSymbol::PointedCap;
			main_line->join_style = LineSymbol::BevelJoin;
		}
		else if (ocad_symbol->ends == 3)
		{
			main_line->cap_style = LineSymbol::PointedCap;
			main_line->join_style = LineSymbol::RoundJoin;
		}
		else if (ocad_symbol->ends == 4)
		{
			main_line->cap_style = LineSymbol::FlatCap;
			main_line->join_style = LineSymbol::MiterJoin;
		}
		else if (ocad_symbol->ends == 6)
		{
			main_line->cap_style = LineSymbol::PointedCap;
			main_line->join_style = LineSymbol::MiterJoin;
		}

		if (main_line->cap_style == LineSymbol::PointedCap)
		{
			if (ocad_symbol->bdist != ocad_symbol->edist)
				addWarning(QObject::tr("In dashed line symbol %1, pointed cap lengths for begin and end are different (%2 and %3). Using %4.")
				.arg(0.1 * ocad_symbol->number).arg(ocad_symbol->bdist).arg(ocad_symbol->edist).arg((ocad_symbol->bdist + ocad_symbol->edist) / 2));
			main_line->pointed_cap_length = convertSize((ocad_symbol->bdist + ocad_symbol->edist) / 2); // FIXME: Different lengths for start and end length of pointed line ends are not supported yet, so take the average
			main_line->join_style = LineSymbol::RoundJoin;	// NOTE: while the setting may be different (see what is set in the first place), OCAD always draws round joins if the line cap is pointed!
		}
		
		// Handle the dash pattern
		if( ocad_symbol->gap > 0 || ocad_symbol->gap2 > 0 )
		{
			main_line->dashed = true;
			
			// Detect special case
			if (ocad_symbol->gap2 > 0 && ocad_symbol->gap == 0)
			{
				main_line->dash_length = convertSize(ocad_symbol->len - ocad_symbol->gap2);
				main_line->break_length = convertSize(ocad_symbol->gap2);
				if (!(ocad_symbol->elen >= ocad_symbol->len / 2 - 1 && ocad_symbol->elen <= ocad_symbol->len / 2 + 1))
					addWarning(QObject::tr("In dashed line symbol %1, the end length cannot be imported correctly.").arg(0.1 * ocad_symbol->number));
				if (ocad_symbol->egap != 0)
					addWarning(QObject::tr("In dashed line symbol %1, the end gap cannot be imported correctly.").arg(0.1 * ocad_symbol->number));
			}
			else
			{
				if (ocad_symbol->len != ocad_symbol->elen)
				{
					if (ocad_symbol->elen >= ocad_symbol->len / 2 - 1 && ocad_symbol->elen <= ocad_symbol->len / 2 + 1)
						main_line->half_outer_dashes = true;
					else
						addWarning(QObject::tr("In dashed line symbol %1, main and end length are different (%2 and %3). Using %4.")
						.arg(0.1 * ocad_symbol->number).arg(ocad_symbol->len).arg(ocad_symbol->elen).arg(ocad_symbol->len));
				}
				
				main_line->dash_length = convertSize(ocad_symbol->len);
				main_line->break_length = convertSize(ocad_symbol->gap);
				if (ocad_symbol->gap2 > 0)
				{
					main_line->dashes_in_group = 2;
					if (ocad_symbol->gap2 != ocad_symbol->egap)
						addWarning(QObject::tr("In dashed line symbol %1, gaps D and E are different (%2 and %3). Using %4.")
						.arg(0.1 * ocad_symbol->number).arg(ocad_symbol->gap2).arg(ocad_symbol->egap).arg(ocad_symbol->gap2));
					main_line->in_group_break_length = convertSize(ocad_symbol->gap2);
					main_line->dash_length = (main_line->dash_length - main_line->in_group_break_length) / 2;
				}
			}
		} 
		else
		{
			main_line->segment_length = convertSize(ocad_symbol->len);
			main_line->end_length = convertSize(ocad_symbol->elen);
		}
	}
	
	// Import a 'double' line?
	LineSymbol *double_line = NULL;
	if (ocad_symbol->dmode != 0)
	{
		double_line = new LineSymbol();
		fillCommonSymbolFields(double_line, (OCADSymbol *)ocad_symbol);
		
		double_line->line_width = convertSize(ocad_symbol->dwidth);
		if (ocad_symbol->dflags & 1)
			double_line->color = convertColor(ocad_symbol->dcolor);
		else
			double_line->color = NULL;
		
		double_line->cap_style = LineSymbol::FlatCap;
		double_line->join_style = LineSymbol::MiterJoin;
		
		double_line->segment_length = convertSize(ocad_symbol->len);
		double_line->end_length = convertSize(ocad_symbol->elen);
		
		// Border lines
		if (ocad_symbol->lwidth > 0 || ocad_symbol->rwidth > 0)
		{
			double_line->have_border_lines = true;
			
			// Border color and width - currently we don't support different values on left and right side,
			// although that seems easy enough to implement in the future. Import with a warning.
			s16 border_color = ocad_symbol->lcolor;
			if (border_color != ocad_symbol->rcolor)
			{
				addWarning(QObject::tr("In symbol %1, left and right borders are different colors (%2 and %3). Using %4.")
				.arg(0.1 * ocad_symbol->number).arg(ocad_symbol->lcolor).arg(ocad_symbol->rcolor).arg(border_color));
			}
			double_line->border_color = convertColor(border_color);
			
			s16 border_width = ocad_symbol->lwidth;
			if (border_width != ocad_symbol->rwidth)
			{
				addWarning(QObject::tr("In symbol %1, left and right borders are different width (%2 and %3). Using %4.")
				.arg(0.1 * ocad_symbol->number).arg(ocad_symbol->lwidth).arg(ocad_symbol->rwidth).arg(border_width));
			}
			double_line->border_width = convertSize(border_width);
			double_line->border_shift = double_line->border_width / 2;
			
			// And finally, the border may be dashed
			if (ocad_symbol->dgap > 0 && ocad_symbol->dmode > 1)
			{
				double_line->dashed_border = true;
				double_line->border_dash_length = convertSize(ocad_symbol->dlen);
				double_line->border_break_length = convertSize(ocad_symbol->dgap);
				
				if (ocad_symbol->dmode == 2)
					addWarning(QObject::tr("In line symbol %1, ignoring that only the left border line should be dashed").arg(0.1 * ocad_symbol->number));
			}
		}
	}
    
    // Create point symbols along line; middle ("normal") dash, corners, start, and end.
    LineSymbol* symbol_line = main_line ? main_line : double_line;	// Find the line to attach the symbols to
    OCADPoint * symbolptr = (OCADPoint *)ocad_symbol->pts;
	symbol_line->mid_symbol = importPattern( ocad_symbol->smnpts, symbolptr);
	symbol_line->mid_symbols_per_spot = ocad_symbol->snum;
	symbol_line->mid_symbol_distance = convertSize(ocad_symbol->sdist);
    symbolptr += ocad_symbol->smnpts;
    if( ocad_symbol->ssnpts > 0 )
    {
		//symbol_line->dash_symbol = importPattern( ocad_symbol->ssnpts, symbolptr);
        symbolptr += ocad_symbol->ssnpts;
    }
    if( ocad_symbol->scnpts > 0 )
    {
		symbol_line->dash_symbol = importPattern( ocad_symbol->scnpts, symbolptr);
        symbol_line->dash_symbol->setName(QObject::tr("Dash symbol"));
        symbolptr += ocad_symbol->scnpts; 
    }
    if( ocad_symbol->sbnpts > 0 )
    {
		symbol_line->start_symbol = importPattern( ocad_symbol->sbnpts, symbolptr);
        symbol_line->start_symbol->setName(QObject::tr("Start symbol"));
        symbolptr += ocad_symbol->sbnpts;
    }
    if( ocad_symbol->senpts > 0 )
    {
		symbol_line->end_symbol = importPattern( ocad_symbol->senpts, symbolptr);
    }
    // FIXME: not really sure how this translates... need test cases
    symbol_line->minimum_mid_symbol_count = 0; //1 + ocad_symbol->smin;
	symbol_line->minimum_mid_symbol_count_when_closed = 0; //1 + ocad_symbol->smin;
	symbol_line->show_at_least_one_symbol = false; // NOTE: this works in a different way than OCAD's 'at least X symbols' setting

    // TODO: taper fields (tmode and tlast)

    if (ocad_symbol->fwidth > 0)
    {
        addWarning(QObject::tr("In symbol %1, ignoring framing line.").arg(0.1 * ocad_symbol->number));
    }
    
    if (main_line == NULL)
		return double_line;
	else if (double_line == NULL)
		return main_line;
	else
	{
		CombinedSymbol* full_line = new CombinedSymbol();
		fillCommonSymbolFields(full_line, (OCADSymbol *)ocad_symbol);
		full_line->setNumParts(2);
		full_line->setPart(0, main_line);
		full_line->setPart(1, double_line);
		
		// Don't let parts be affected by possible settings for the combined symbol
		main_line->setHidden(false);
		main_line->setProtected(false);
		double_line->setHidden(false);
		double_line->setProtected(false);
		
		return full_line;
	}
}

Symbol *OCAD8FileImport::importAreaSymbol(const OCADAreaSymbol *ocad_symbol)
{
    AreaSymbol *symbol = new AreaSymbol();
    fillCommonSymbolFields(symbol, (OCADSymbol *)ocad_symbol);

    // Basic area symbol fields: minimum_area, color
    symbol->minimum_area = 0;
    symbol->color = ocad_symbol->fill ? convertColor(ocad_symbol->color) : NULL;
    symbol->patterns.clear();
    AreaSymbol::FillPattern *pat = NULL;

    // Hatching
    if (ocad_symbol->hmode > 0)
    {
        int n = symbol->patterns.size(); symbol->patterns.resize(n + 1); pat = &(symbol->patterns[n]);
        pat->type = AreaSymbol::FillPattern::LinePattern;
        pat->angle = convertRotation(ocad_symbol->hangle1);
        pat->rotatable = true;
        pat->line_spacing = convertSize(ocad_symbol->hdist + ocad_symbol->hwidth);
        pat->line_offset = 0;
        pat->line_color = convertColor(ocad_symbol->hcolor);
        pat->line_width = convertSize(ocad_symbol->hwidth);
        if (ocad_symbol->hmode == 2)
        {
            // Second hatch, same as the first, just a different angle
            int n = symbol->patterns.size(); symbol->patterns.resize(n + 1); pat = &(symbol->patterns[n]);
            pat->type = AreaSymbol::FillPattern::LinePattern;
            pat->angle = convertRotation(ocad_symbol->hangle2);
            pat->rotatable = true;
            pat->line_spacing = convertSize(ocad_symbol->hdist);
            pat->line_offset = 0;
            pat->line_color = convertColor(ocad_symbol->hcolor);
            pat->line_width = convertSize(ocad_symbol->hwidth);
        }
    }

    if (ocad_symbol->pmode > 0)
    {
        // OCAD 8 has a "staggered" pattern mode, where successive rows are shifted width/2 relative
        // to each other. We need to simulate this in Mapper with two overlapping patterns, each with
        // twice the height. The second is then offset by width/2, height/2.
        qint64 spacing = convertSize(ocad_symbol->pheight);
        if (ocad_symbol->pmode == 2) spacing *= 2;
        int n = symbol->patterns.size(); symbol->patterns.resize(n + 1); pat = &(symbol->patterns[n]);
        pat->type = AreaSymbol::FillPattern::PointPattern;
        pat->angle = convertRotation(ocad_symbol->pangle);
        pat->rotatable = true;
        pat->point_distance = convertSize(ocad_symbol->pwidth);
        pat->line_spacing = spacing;
        pat->line_offset = 0;
        pat->offset_along_line = 0;
        // FIXME: somebody needs to own this symbol and be responsible for deleting it
        // Right now it looks like a potential memory leak
        pat->point = importPattern(ocad_symbol->npts, (OCADPoint *)ocad_symbol->pts);
        if (ocad_symbol->pmode == 2)
        {
            int n = symbol->patterns.size(); symbol->patterns.resize(n + 1); pat = &(symbol->patterns[n]);
            pat->type = AreaSymbol::FillPattern::PointPattern;
            pat->angle = convertRotation(ocad_symbol->pangle);
            pat->rotatable = true;
            pat->point_distance = convertSize(ocad_symbol->pwidth);
            pat->line_spacing = spacing;
            pat->line_offset = pat->line_spacing / 2;
            pat->offset_along_line = pat->point_distance / 2;
            pat->point = importPattern(ocad_symbol->npts, (OCADPoint *)ocad_symbol->pts);
        }
    }

    return symbol;
}

Symbol *OCAD8FileImport::importTextSymbol(const OCADTextSymbol *ocad_symbol)
{
    TextSymbol *symbol = new TextSymbol();
    fillCommonSymbolFields(symbol, (OCADSymbol *)ocad_symbol);

    symbol->font_family = convertPascalString(ocad_symbol->font); // FIXME: font mapping?
    symbol->color = convertColor(ocad_symbol->color);
	double d_font_size = (0.1 * ocad_symbol->dpts) / 72.0 * 25.4;
	symbol->font_size = qRound(1000 * d_font_size);
    symbol->bold = (ocad_symbol->bold >= 550) ? true : false;
    symbol->italic = (ocad_symbol->italic) ? true : false;
    symbol->underline = false;
	symbol->paragraph_spacing = convertSize(ocad_symbol->pspace);
	symbol->character_spacing = ocad_symbol->cspace / 100.0;
	symbol->kerning = false;
	symbol->line_below = ocad_symbol->under;
	symbol->line_below_color = convertColor(ocad_symbol->ucolor);
	symbol->line_below_width = convertSize(ocad_symbol->uwidth);
	symbol->line_below_distance = convertSize(ocad_symbol->udist);
	symbol->custom_tabs.resize(ocad_symbol->ntabs);
	for (int i = 0; i < ocad_symbol->ntabs; ++i)
		symbol->custom_tabs[i] = convertSize(ocad_symbol->tab[i]);
	
	int halign = (int)TextObject::AlignHCenter;
	if (ocad_symbol->halign == 0)
		halign = (int)TextObject::AlignLeft;
	else if (ocad_symbol->halign == 1)
		halign = (int)TextObject::AlignHCenter;
	else if (ocad_symbol->halign == 2)
		halign = (int)TextObject::AlignRight;
	else if (ocad_symbol->halign == 3)
	{
		// TODO: implement justified alignment
		addWarning(QObject::tr("During import of text symbol %1: ignoring justified alignment").arg(0.1 * ocad_symbol->number));
	}
	text_halign_map[symbol] = halign;

    if (ocad_symbol->bold != 400 && ocad_symbol->bold != 700)
    {
        addWarning(QObject::tr("During import of text symbol %1: ignoring custom weight (%2)")
                       .arg(0.1 * ocad_symbol->number).arg(ocad_symbol->bold));
    }
    if (ocad_symbol->cspace != 0)
	{
		addWarning(QObject::tr("During import of text symbol %1: custom character spacing is set, its implementation does not match OCAD's behavior yet")
		.arg(0.1 * ocad_symbol->number));
	}
    if (ocad_symbol->wspace != 100)
    {
        addWarning(QObject::tr("During import of text symbol %1: ignoring custom word spacing (%2%)")
                       .arg(0.1 * ocad_symbol->number).arg(ocad_symbol->wspace));
    }
    if (ocad_symbol->indent1 != 0 || ocad_symbol->indent2 != 0)
    {
        addWarning(QObject::tr("During import of text symbol %1: ignoring custom indents (%2/%3)")
                       .arg(0.1 * ocad_symbol->number).arg(ocad_symbol->indent1).arg(ocad_symbol->indent2));
    }
	
	if (ocad_symbol->fmode > 0)
	{
		symbol->framing = true;
		symbol->framing_color = convertColor(ocad_symbol->fcolor);
		if (ocad_symbol->fmode == 1)
		{
			symbol->framing_mode = TextSymbol::ShadowFraming;
			symbol->framing_shadow_x_offset = convertSize(ocad_symbol->fdx);
			symbol->framing_shadow_y_offset = -1 * convertSize(ocad_symbol->fdy);
		}
		else if (ocad_symbol->fmode == 2)
		{
			symbol->framing_mode = TextSymbol::LineFraming;
			symbol->framing_line_half_width = convertSize(ocad_symbol->fdpts);
		}
		else
		{
			addWarning(QObject::tr("During import of text symbol %1: ignoring text framing (mode %2)")
			.arg(0.1 * ocad_symbol->number).arg(ocad_symbol->fmode));
		}
	}

    symbol->updateQFont();
	
	// Convert line spacing
	double absolute_line_spacing = d_font_size * 0.01 * ocad_symbol->lspace;
	symbol->line_spacing = absolute_line_spacing / (symbol->getFontMetrics().lineSpacing() / symbol->calculateInternalScaling());

    return symbol;
}
OCAD8FileImport::RectangleInfo* OCAD8FileImport::importRectSymbol(const OCADRectSymbol* ocad_symbol)
{
	RectangleInfo rect;
	rect.border_line = new LineSymbol();
	fillCommonSymbolFields(rect.border_line, (OCADSymbol *)ocad_symbol);
	rect.border_line->line_width = convertSize(ocad_symbol->width);
	rect.border_line->color = convertColor(ocad_symbol->color);
	rect.border_line->cap_style = LineSymbol::FlatCap;
	rect.border_line->join_style = LineSymbol::RoundJoin;
	rect.corner_radius = 0.001 * convertSize(ocad_symbol->corner);
	rect.has_grid = ocad_symbol->flags & 1;
	
	if (rect.has_grid)
	{
		rect.inner_line = new LineSymbol();
		fillCommonSymbolFields(rect.inner_line, (OCADSymbol *)ocad_symbol);
		rect.inner_line->setNumberComponent(2, 1);
		rect.inner_line->line_width = qRound(1000 * 0.15);
		rect.inner_line->color = rect.border_line->color;
		
		rect.text = new TextSymbol();
		fillCommonSymbolFields(rect.text, (OCADSymbol *)ocad_symbol);
		rect.text->setNumberComponent(2, 2);
		rect.text->font_family = "Arial";
		rect.text->font_size = qRound(1000 * (15 / 72.0 * 25.4));
		rect.text->color = rect.border_line->color;
		rect.text->bold = true;
		rect.text->updateQFont();
		
		rect.number_from_bottom = ocad_symbol->flags & 2;
		rect.cell_width = 0.001 * convertSize(ocad_symbol->cwidth);
		rect.cell_height = 0.001 * convertSize(ocad_symbol->cheight);
		rect.unnumbered_cells = ocad_symbol->gcells;
		rect.unnumbered_text = convertPascalString(ocad_symbol->gtext);
	}
	
	return &rectangle_info.insert(ocad_symbol->number, rect).value();
}

PointSymbol *OCAD8FileImport::importPattern(s16 npts, OCADPoint *pts)
{
    PointSymbol *symbol = new PointSymbol();
    symbol->rotatable = true;
    OCADPoint *p = pts, *end = pts + npts;
    while (p < end) {
        OCADSymbolElement *elt = (OCADSymbolElement *)p;
        int element_index = symbol->getNumElements();
		bool multiple_elements = p + (2 + elt->npts) < end || p > pts;
        if (elt->type == OCAD_DOT_ELEMENT)
        {
			int inner_radius = (int)convertSize(elt->diameter) / 2;
			if (inner_radius > 0)
			{
				PointSymbol* element_symbol = multiple_elements ? (new PointSymbol()) : symbol;
				element_symbol->inner_color = convertColor(elt->color);
				element_symbol->inner_radius = inner_radius;
				element_symbol->outer_color = NULL;
				element_symbol->outer_width = 0;
				if (multiple_elements)
				{
					element_symbol->rotatable = false;
					PointObject* element_object = new PointObject(element_symbol);
					element_object->coords.resize(1);
					symbol->addElement(element_index, element_object, element_symbol);
				}
			}
        }
        else if (elt->type == OCAD_CIRCLE_ELEMENT)
        {
			int inner_radius = (int)convertSize(elt->diameter) / 2 - (int)convertSize(elt->width);
			int outer_width = (int)convertSize(elt->width);
			if (outer_width > 0 && inner_radius > 0)
			{
				PointSymbol* element_symbol = (multiple_elements) ? (new PointSymbol()) : symbol;
				element_symbol->inner_color = NULL;
				element_symbol->inner_radius = inner_radius;
				element_symbol->outer_color = convertColor(elt->color);
				element_symbol->outer_width = outer_width;
				if (multiple_elements)
				{
					element_symbol->rotatable = false;
					PointObject* element_object = new PointObject(element_symbol);
					element_object->coords.resize(1);
					symbol->addElement(element_index, element_object, element_symbol);
				}
			}
        }
        else if (elt->type == OCAD_LINE_ELEMENT)
        {
            LineSymbol* element_symbol = new LineSymbol();
            element_symbol->line_width = convertSize(elt->width);
            element_symbol->color = convertColor(elt->color);
            PathObject* element_object = new PathObject(element_symbol);
            fillPathCoords(element_object, false, elt->npts, elt->pts);
			element_object->recalculateParts();
            symbol->addElement(element_index, element_object, element_symbol);
        }
        else if (elt->type == OCAD_AREA_ELEMENT)
        {
            AreaSymbol* element_symbol = new AreaSymbol();
            element_symbol->color = convertColor(elt->color);
            PathObject* element_object = new PathObject(element_symbol);
            fillPathCoords(element_object, true, elt->npts, elt->pts);
			element_object->recalculateParts();
            symbol->addElement(element_index, element_object, element_symbol);
        }
        p += (2 + elt->npts);
    }
    return symbol;
}


void OCAD8FileImport::fillCommonSymbolFields(Symbol *symbol, const OCADSymbol *ocad_symbol)
{
    // common fields are name, number, description, helper_symbol, hidden/protected status
    symbol->name = convertPascalString(ocad_symbol->name);
    symbol->number[0] = ocad_symbol->number / 10;
    symbol->number[1] = ocad_symbol->number % 10;
    symbol->number[2] = -1;
    symbol->is_helper_symbol = false; // no such thing in OCAD
    if (ocad_symbol->status & 1)
		symbol->setProtected(true);
	if (ocad_symbol->status & 2)
		symbol->setHidden(true);
}

Object *OCAD8FileImport::importObject(const OCADObject* ocad_object, MapLayer* layer)
{
	Symbol* symbol;
    if (!symbol_index.contains(ocad_object->symbol))
    {
		if (!rectangle_info.contains(ocad_object->symbol))
		{
			if (ocad_object->type == 1)
				symbol = map->getUndefinedPoint();
			else if (ocad_object->type == 2 || ocad_object->type == 3)
				symbol = map->getUndefinedLine();
			else
			{
				addWarning(QObject::tr("Unable to load object"));
				return NULL;
			}
		}
		else
		{
			if (!importRectangleObject(ocad_object, layer, rectangle_info[ocad_object->symbol]))
				addWarning(QObject::tr("Unable to import rectangle object"));
			return NULL;
		}
    }
    else
		symbol = symbol_index[ocad_object->symbol];

    Object *object = NULL;
    if (symbol->getType() == Symbol::Point)
    {
        PointObject *p = new PointObject();
        p->symbol = symbol;

        // extra properties: rotation
		PointSymbol* point_symbol = reinterpret_cast<PointSymbol*>(symbol);
		if (point_symbol->isRotatable())
			p->setRotation(convertRotation(ocad_object->angle));
		else if (ocad_object->angle != 0)
		{
			if (!point_symbol->isSymmetrical())
			{
				point_symbol->setRotatable(true);
				p->setRotation(convertRotation(ocad_object->angle));
			}
		}

        // only 1 coordinate is allowed, enforce it even if the OCAD object claims more.
        fillPathCoords(p, false, 1, (OCADPoint *)ocad_object->pts);
        p->setMap(map);
        return p;
    }
    else if (symbol->getType() == Symbol::Text)
    {
		TextObject *t = new TextObject(symbol);

        // extra properties: rotation, horizontalAlignment, verticalAlignment, text
        t->setRotation(convertRotation(ocad_object->angle));
		t->setHorizontalAlignment((TextObject::HorizontalAlignment)text_halign_map.value(symbol));
		t->setVerticalAlignment(TextObject::AlignBaseline);

        const char *text_ptr = (const char *)(ocad_object->pts + ocad_object->npts);
        size_t text_len = sizeof(OCADPoint) * ocad_object->ntext;
        if (ocad_object->unicode) t->setText(convertWideCString(text_ptr, text_len, true));
        else t->setText(convertCString(text_ptr, text_len, true));

        // Text objects need special path translation
        if (!fillTextPathCoords(t, reinterpret_cast<TextSymbol*>(symbol), ocad_object->npts, (OCADPoint *)ocad_object->pts))
        {
            addWarning(QObject::tr("Not importing text symbol, couldn't figure out path' (npts=%1): %2")
                           .arg(ocad_object->npts).arg(t->getText()));
            delete t;
            return NULL;
        }
        t->setMap(map);
        return t;
    }
    else if (symbol->getType() == Symbol::Line || symbol->getType() == Symbol::Area || symbol->getType() == Symbol::Combined) {
		PathObject *p = new PathObject(symbol);

		// Normal path
		fillPathCoords(p, symbol->getType() == Symbol::Area, ocad_object->npts, (OCADPoint *)ocad_object->pts);
		p->recalculateParts();
		p->setMap(map);
		return p;
    }

    if (object == NULL) return NULL;

    // Set some common fields
    object->map = map;
    object->output_dirty = true;

    return object;
}

bool OCAD8FileImport::importRectangleObject(const OCADObject* ocad_object, MapLayer* layer, const OCAD8FileImport::RectangleInfo& rect)
{
	if (ocad_object->npts != 4)
		return false;
	
	// Convert corner points
	s32 buf[3];
	ocad_point(buf, &(ocad_object->pts[3]));
	MapCoord top_left;
	convertPoint(top_left, buf[0], buf[1]);
	ocad_point(buf, &(ocad_object->pts[0]));
	MapCoord bottom_left;
	convertPoint(bottom_left, buf[0], buf[1]);
	ocad_point(buf, &(ocad_object->pts[2]));
	MapCoord top_right;
	convertPoint(top_right, buf[0], buf[1]);
	ocad_point(buf, &(ocad_object->pts[1]));
	MapCoord bottom_right;
	convertPoint(bottom_right, buf[0], buf[1]);
	
	MapCoordF top_left_f = MapCoordF(top_left);
	MapCoordF top_right_f = MapCoordF(top_right);
	MapCoordF bottom_left_f = MapCoordF(bottom_left);
	MapCoordF bottom_right_f = MapCoordF(bottom_right);
	MapCoordF right = MapCoordF(top_right.xd() - top_left.xd(), top_right.yd() - top_left.yd());
	double angle = right.getAngle();
	MapCoordF down = MapCoordF(bottom_left.xd() - top_left.xd(), bottom_left.yd() - top_left.yd());
	right.normalize();
	down.normalize();
	
	// Create border line
	MapCoordVector coords;
	if (rect.corner_radius == 0)
	{
		coords.push_back(top_left);
		coords.push_back(top_right);
		coords.push_back(bottom_right);
		coords.push_back(bottom_left);
	}
	else
	{
		double handle_radius = (1 - BEZIER_KAPPA) * rect.corner_radius;
		coords.push_back((top_right_f - right * rect.corner_radius).toCurveStartMapCoord());
		coords.push_back((top_right_f - right * handle_radius).toMapCoord());
		coords.push_back((top_right_f + down * handle_radius).toMapCoord());
		coords.push_back((top_right_f + down * rect.corner_radius).toMapCoord());
		coords.push_back((bottom_right_f - down * rect.corner_radius).toCurveStartMapCoord());
		coords.push_back((bottom_right_f - down * handle_radius).toMapCoord());
		coords.push_back((bottom_right_f - right * handle_radius).toMapCoord());
		coords.push_back((bottom_right_f - right * rect.corner_radius).toMapCoord());
		coords.push_back((bottom_left_f + right * rect.corner_radius).toCurveStartMapCoord());
		coords.push_back((bottom_left_f + right * handle_radius).toMapCoord());
		coords.push_back((bottom_left_f - down * handle_radius).toMapCoord());
		coords.push_back((bottom_left_f - down * rect.corner_radius).toMapCoord());
		coords.push_back((top_left_f + down * rect.corner_radius).toCurveStartMapCoord());
		coords.push_back((top_left_f + down * handle_radius).toMapCoord());
		coords.push_back((top_left_f + right * handle_radius).toMapCoord());
		coords.push_back((top_left_f + right * rect.corner_radius).toMapCoord());
	}
	PathObject *border_path = new PathObject(rect.border_line, coords, map);
	border_path->getPart(0).setClosed(true, false);
	layer->objects.push_back(border_path);
	
	if (rect.has_grid && rect.cell_width > 0 && rect.cell_height > 0)
	{
		// Calculate grid sizes
		double width = top_left.lengthTo(top_right);
		double height = top_left.lengthTo(bottom_left);
		int num_cells_x = qMax(1, qRound(width / rect.cell_width));
		int num_cells_y = qMax(1, qRound(height / rect.cell_height));
		
		float cell_width = width / num_cells_x;
		float cell_height = height / num_cells_y;
		
		// Create grid lines
		coords.resize(2);
		for (int x = 1; x < num_cells_x; ++x)
		{
			coords[0] = (top_left_f + x * cell_width * right).toMapCoord();
			coords[1] = (bottom_left_f + x * cell_width * right).toMapCoord();
			
			PathObject *path = new PathObject(rect.inner_line, coords, map);
			layer->objects.push_back(path);
		}
		for (int y = 1; y < num_cells_y; ++y)
		{
			coords[0] = (top_left_f + y * cell_height * down).toMapCoord();
			coords[1] = (top_right_f + y * cell_height * down).toMapCoord();
			
			PathObject *path = new PathObject(rect.inner_line, coords, map);
			layer->objects.push_back(path);
		}
		
		// Create grid text
		if (height >= rect.cell_height / 2)
		{
			for (int y = 0; y < num_cells_y; ++y) 
			{
				for (int x = 0; x < num_cells_x; ++x)
				{
					int cell_num;
					QString cell_text;
					
					if (rect.number_from_bottom)
						cell_num = y * num_cells_x + x + 1;
					else
						cell_num = (num_cells_y - 1 - y) * num_cells_x + x + 1;
					
					if (cell_num > num_cells_x * num_cells_y - rect.unnumbered_cells)
						cell_text = rect.unnumbered_text;
					else
						cell_text = QString::number(cell_num);
					
					TextObject* object = new TextObject(rect.text);
					object->setMap(map);
					object->setText(cell_text);
					object->setRotation(-angle);
					object->setHorizontalAlignment(TextObject::AlignLeft);
					object->setVerticalAlignment(TextObject::AlignTop);
					double position_x = (x + 0.07f) * cell_width;
					double position_y = (y + 0.04f) * cell_height + rect.text->getFontMetrics().ascent() / rect.text->calculateInternalScaling() - rect.text->getFontSize();
					object->setAnchorPosition(top_left_f + position_x * right + position_y * down);
					layer->objects.push_back(object);
					
					//pts[0].Y -= rectinfo.gridText.FontAscent - rectinfo.gridText.FontEmHeight;
				}
			}
		}
	}
	
	return true;
}

void OCAD8FileImport::importString(OCADStringEntry *entry)
{
    OCADCString *ocad_str = ocad_string(file, entry);
    if (entry->type == 8)
    {
		// Template
		// TODO: also parse map templates
        OCADBackground background;
		if (ocad_to_background(&background, ocad_str) == 0)
        {
            Template* templ = importRasterTemplate(background);
			if (templ)
			{
				map->templates.push_back(templ);
				view->getTemplateVisibility(templ)->visible = true;
			}
        }
        else
			addWarning(QObject::tr("Unable to import template: %1").arg(ocad_str->str));
    }
    // FIXME: parse more types of strings, maybe the print parameters?
    
    return;
}

Template *OCAD8FileImport::importRasterTemplate(const OCADBackground &background)
{
    QString filename(background.filename); // FIXME: use platform char encoding?
    if (isRasterImageFile(filename))
    {
        TemplateImage *templ = new TemplateImage(filename, map);
        MapCoord c;
        convertPoint(c, background.trnx, background.trny);
        templ->setTemplateX(c.rawX());
        templ->setTemplateY(c.rawY());
        // This seems to be measured in degrees. Plus there's wacky values like -359.7.
        templ->setTemplateRotation(M_PI / 180 * background.angle);
        templ->setTemplateScaleX(convertTemplateScale(background.sclx));
        templ->setTemplateScaleY(convertTemplateScale(background.scly));
        // FIXME: import template view parameters: background.dimming and background.transparent
        return templ;
    }
    else
    {
        addWarning(QObject::tr("Unable to import template: background \"%1\" doesn't seem to be a raster image").arg(filename));
    }
    return NULL;
}

bool OCAD8FileImport::isRasterImageFile(const QString &filename) const
{
    int dot_pos = filename.lastIndexOf('.');
	if (dot_pos < 0)
		return false;
	QString extension = filename.right(filename.length() - dot_pos - 1).toLower();
    return QImageReader::supportedImageFormats().contains(extension.toAscii());
}

/** Translates the OCAD path given in the last two arguments into an Object.
 */
void OCAD8FileImport::fillPathCoords(Object *object, bool is_area, s16 npts, OCADPoint *pts)
{
    object->coords.resize(npts);
    s32 buf[3];
    for (int i = 0; i < npts; i++)
    {
        ocad_point(buf, &(pts[i]));
        MapCoord &coord = object->coords[i];
        convertPoint(coord, buf[0], buf[1]);
        // We can support CurveStart, HolePoint, DashPoint.
        // CurveStart needs to be applied to the main point though, not the control point, and
		// hole points need to bet set as the last point of a part of an area object instead of the first point of the next part
        if (buf[2] & PX_CTL1 && i > 0) object->coords[i-1].setCurveStart(true);
		if ((buf[2] & (PY_DASH << 8)) || (buf[2] & (PY_CORNER << 8))) coord.setDashPoint(true);
        if (buf[2] & (PY_HOLE << 8))
		{
			if (is_area)
				object->coords[i-1].setHolePoint(true);
			else
				coord.setHolePoint(true);
		}
    }
    
    // For path objects, create closed parts where the position of the last point is equal to that of the first point
    if (object->getType() == Object::Path)
	{
		int start = 0;
		for (int i = 0; i < (int)object->coords.size(); ++i)
		{
			if (!object->coords[i].isHolePoint() && i < (int)object->coords.size() - 1)
				continue;
			
			if (object->coords[i].isPositionEqualTo(object->coords[start]))
				object->coords[i].setClosePoint(true);
			
			start = i + 1;
		}
	}
}

/** Translates an OCAD text object path into a Mapper text object specifier, if possible.
 *  If successful, sets either 1 or 2 coordinates in the text object and returns true.
 *  If the OCAD path was not importable, leaves the TextObject alone and returns false.
 */
bool OCAD8FileImport::fillTextPathCoords(TextObject *object, TextSymbol *symbol, s16 npts, OCADPoint *pts)
{
    // text objects either have 1 point (free anchor) or 2 (midpoint/size)
    // OCAD appears to always have 5 or 4 points (possible single anchor, then 4 corner coordinates going clockwise from anchor).
    if (npts == 0) return false;
	
	if (npts == 4)
	{
		// Box text
		s32 buf[3];
		ocad_point(buf, &(pts[3]));
		MapCoord top_left;
		convertPoint(top_left, buf[0], buf[1]);
		ocad_point(buf, &(pts[0]));
		MapCoord bottom_left;
		convertPoint(bottom_left, buf[0], buf[1]);
		ocad_point(buf, &(pts[2]));
		MapCoord top_right;
		convertPoint(top_right, buf[0], buf[1]);
		
		// According to Purple Pen source code: OCAD adds an extra internal leading (incorrectly).
		QFontMetricsF metrics = symbol->getFontMetrics();
		double top_adjust = -symbol->getFontSize() + (metrics.ascent() + metrics.descent() + 0.5) / symbol->calculateInternalScaling();
		
		MapCoordF adjust_vector = MapCoordF(top_adjust * sin(object->getRotation()), top_adjust * cos(object->getRotation()));
		top_left = MapCoord(top_left.xd() + adjust_vector.getX(), top_left.yd() + adjust_vector.getY());
		bottom_left = MapCoord(bottom_left.xd() + adjust_vector.getX(), bottom_left.yd() + adjust_vector.getY());
		top_right = MapCoord(top_right.xd() + adjust_vector.getX(), top_right.yd() + adjust_vector.getY());
		
		object->setBox((bottom_left.rawX() + top_right.rawX()) / 2, (bottom_left.rawY() + top_right.rawY()) / 2,
					   top_left.lengthTo(top_right), top_left.lengthTo(bottom_left));

		object->setVerticalAlignment(TextObject::AlignTop);
	}
	else
	{
		// Single anchor text
		if (npts != 5)
			addWarning(QObject::tr("Trying to import a text object with unknown coordinate format"));
		
		s32 buf[3];
		ocad_point(buf, &(pts[0])); // anchor point
		
		MapCoord coord;
		convertPoint(coord, buf[0], buf[1]);
		object->setAnchorPosition(coord.rawX(), coord.rawY());
		
		object->setVerticalAlignment(TextObject::AlignBaseline);
	}

    return true;
}

/** Converts a single-byte-per-character, length-payload string to a QString.
 *
 *  The byte sequence will be: LEN C0 C1 C2 C3...
 *
 *  Obviously this will only hold up to 255 characters. By default, we interpret the
 *  bytes using Windows-1252, as that's the most likely candidate for OCAD files in
 *  the wild.
 */
QString OCAD8FileImport::convertPascalString(const char *p) {
    int len = *((unsigned char *)p);
    return encoding_1byte->toUnicode((p + 1), len);
}

/** Converts a single-byte-per-character, zero-terminated string to a QString.
 *
 *  The byte sequence will be: C0 C1 C2 C3 ... 00. n describes the maximum
 *  length (in bytes) that will be scanned for a zero terminator; if none is found,
 *  the string will be truncated at that location.
 */
QString OCAD8FileImport::convertCString(const char *p, size_t n, bool ignore_first_newline) {
    size_t i = 0;
    for (; i < n; i++) {
        if (p[i] == 0) break;
    }
    if (ignore_first_newline && n >= 2 && p[0] == '\r' && p[1] == '\n')
	{
		// Remove "\r\n" at the beginning of texts, somehow OCAD seems to add this sometimes but ignores it
		p += 2;
		i -= 2;
	}
    return encoding_1byte->toUnicode(p, i);
}

/** Converts a two-byte-per-character, zero-terminated string to a QString. By default,
 *  we interpret the bytes using UTF-16LE, as that's the most likely candidate for
 *  OCAD files in the wild.
 *
 *  The byte sequence will be: L0 H0 L1 H1 L2 H2... 00 00. n describes the maximum
 *  length (in bytes) that will be scanned for a zero terminator; if none is found,
 *  the string will be truncated at that location.
 */
QString OCAD8FileImport::convertWideCString(const char *p, size_t n, bool ignore_first_newline) {
    const u16 *q = (const u16 *)p;
    size_t i = 0;
    for (; i < n; i++) {
        if (q[i] == 0) break;
    }
    if (ignore_first_newline && n >= 4 && p[0] == '\r' && p[2] == '\n')
	{
		// Remove "\r\n" at the beginning of texts, somehow OCAD seems to add this sometimes but ignores it
		p += 4;
		i -= 2;
	}
    return encoding_2byte->toUnicode(p, i * 2);
}

float OCAD8FileImport::convertRotation(int angle) {
    // OCAD uses tenths of a degree, counterclockwise
    // BUG: if sin(rotation) is < 0 for a hatched area pattern, the pattern's createRenderables() will go into an infinite loop.
    // So until that's fixed, we keep a between 0 and PI
    double a = (M_PI / 180) *  (0.1f * angle);
    while (a < 0) a += 2 * M_PI;
    //if (a < 0 || a > M_PI) qDebug() << "Found angle" << a;
    return (float)a;
}

void OCAD8FileImport::convertPoint(MapCoord &coord, int ocad_x, int ocad_y)
{
    // OCAD uses hundredths of a millimeter.
    // oo-mapper uses 1/1000 mm
    coord.setRawX(offset_x + (qint64)ocad_x * 10);
    // Y-axis is flipped.
    coord.setRawY(offset_y + (qint64)ocad_y * (-10));
}

qint64 OCAD8FileImport::convertSize(int ocad_size) {
    // OCAD uses hundredths of a millimeter.
    // oo-mapper uses 1/1000 mm
    return ((qint64)ocad_size) * 10;
}

MapColor *OCAD8FileImport::convertColor(int color) {
	if (!color_index.contains(color))
	{
		addWarning(QObject::tr("Color id not found: %1, ignoring this color").arg(color));
		return NULL;
	}
	else
		return color_index[color];
}

double OCAD8FileImport::convertTemplateScale(double ocad_scale)
{
	double mpd = ocad_scale * 0.00001;			// meters(on map) per pixel
	return mpd * map->getScaleDenominator();	// meters(in reality) per pixel
}



OCAD8FileExport::OCAD8FileExport(QIODevice* stream, Map* map, MapView* view) : Exporter(stream, map, view), file(NULL)
{
	ocad_init();
	encoding_1byte = QTextCodec::codecForName("Windows-1252");
	encoding_2byte = QTextCodec::codecForName("UTF-16LE");
	
	origin_point_object = new PointObject();
}

OCAD8FileExport::~OCAD8FileExport()
{
	delete origin_point_object;
}

void OCAD8FileExport::doExport() throw (FormatException)
{
	if (map->getNumColors() > 256)
		throw FormatException(QObject::tr("The map contains more than 256 colors which is not supported by ocd version 8."));
	
	// Create struct in memory
	int err = ocad_file_new(&file);
	if (err != 0) throw FormatException(QObject::tr("Could not create new file: libocad returned %1").arg(err));
	
	// Fill header struct
	OCADFileHeader* header = file->header;
	*(((u8*)&header->magic) + 0) = 0xAD;
	*(((u8*)&header->magic) + 1) = 0x0C;
	header->ftype = 2;
	header->major = 8;
	header->minor = 0;
	if (map->getMapNotes().size() > 0)
	{
		header->infosize = map->getMapNotes().length() + 1;
		ocad_file_reserve(file, header->infosize);
		header->infopos = &file->buffer[file->size] - file->buffer;
		convertCString(map->getMapNotes(), &file->buffer[file->size], header->infosize);
		file->size += header->infosize;
	}
	
	// Fill setup struct
	OCADSetup* setup = file->setup;
	if (view)
	{
		setup->center = convertPoint(view->getPositionX(), view->getPositionY());
		setup->zoom = view->getZoom();
	}
	else
		setup->zoom = 1;
	setup->scale = map->getScaleDenominator();
	
	// TODO: GPS projection parameters
	
	// TODO: print parameters
	
	// Colors
	for (int i = 0; i < map->getNumColors(); i++)
	{
		++file->header->ncolors;
		OCADColor *ocad_color = ocad_color_at(file, i);
		
		MapColor* color = map->getColor(i);
		
		ocad_color->number = i;
		ocad_color->cyan = qRound(1 / 0.005f * color->c);
		ocad_color->magenta = qRound(1 / 0.005f * color->m);
		ocad_color->yellow = qRound(1 / 0.005f * color->y);
		ocad_color->black = qRound(1 / 0.005f * color->k);
		convertPascalString(color->name, ocad_color->name, 32);
		// ocad_color->spot
	}
	
	// Symbols
	for (int i = 0; i < map->getNumSymbols(); ++i)
	{
		Symbol* symbol = map->getSymbol(i);
		
		s16 index = -1;
		if (symbol->getType() == Symbol::Point)
			index = exportPointSymbol(static_cast<PointSymbol*>(symbol));
		else if (symbol->getType() == Symbol::Line)
			index = exportLineSymbol(static_cast<LineSymbol*>(symbol));
		else if (symbol->getType() == Symbol::Area)
			index = exportAreaSymbol(static_cast<AreaSymbol*>(symbol));
		else if (symbol->getType() == Symbol::Text)
			index = exportTextSymbol(static_cast<TextSymbol*>(symbol));
		else if (symbol->getType() == Symbol::Combined)
			; // This is done as a second pass to ensure that all dependencies are added to the symbol_index
		else
			assert(false);
		
		if (index >= 0)
		{
			std::set<s16> number;
			number.insert(index);
			symbol_index.insert(symbol, number);
		}
	}
	
	for (int i = 0; i < map->getNumSymbols(); ++i)
	{
		Symbol* symbol = map->getSymbol(i);
		
		if (symbol->getType() == Symbol::Combined)
			symbol_index.insert(symbol, exportCombinedSymbol(static_cast<CombinedSymbol*>(symbol)));
	}
	
	// Objects
	OCADObject* ocad_object = ocad_object_alloc(NULL);
	for (int l = 0; l < map->getNumLayers(); ++l)
	{
		for (int o = 0; o < map->getLayer(l)->getNumObjects(); ++o)
		{
			memset(ocad_object, 0, sizeof(OCADObject) - sizeof(OCADPoint) + 8 * (ocad_object->npts + ocad_object->ntext));
			Object* object = map->getLayer(l)->getObject(o);
			object->update();
			
			// Fill some common entries of object struct
			OCADPoint* coord_buffer = ocad_object->pts;
			if (object->getType() != Object::Text)
				ocad_object->npts = exportCoordinates(object->getRawCoordinateVector(), &coord_buffer, object->getSymbol());
			else
				ocad_object->npts = exportTextCoordinates(static_cast<TextObject*>(object), &coord_buffer);
			
			if (object->getType() == Object::Point)
			{
				PointObject* point = static_cast<PointObject*>(object);
				ocad_object->angle = convertRotation(point->getRotation());
			}
			else if (object->getType() == Object::Text)
			{
				TextObject* text = static_cast<TextObject*>(object);
				ocad_object->unicode = 1;
				ocad_object->angle = convertRotation(text->getRotation());
				int num_letters = convertWideCString(text->getText(), (unsigned char*)coord_buffer, 8 * (OCAD_MAX_OBJECT_PTS - ocad_object->npts));
				ocad_object->ntext = qCeil(num_letters / 4.0f);
			}
			
			// Insert an object into the map for every symbol contained in the symbol_index
			std::set<s16> index_set;
			if (symbol_index.contains(object->getSymbol()))
				index_set = symbol_index[object->getSymbol()];
			else
				index_set.insert(-1);	// export as undefined symbol; TODO: is this the correct symbol number?
			
			for (std::set<s16>::const_iterator it = index_set.begin(), end = index_set.end(); it != end; ++it)
			{
				s16 index_to_use = *it;
				
				// For text objects, check if we have to change / create a new text symbol because of the formatting
				if (object->getType() == Object::Text)
				{
					TextObject* text_object = static_cast<TextObject*>(object);
					TextSymbol* text_symbol = static_cast<TextSymbol*>(object->getSymbol());
					if (!text_format_map.contains(text_symbol))
					{
						// Adjust the formatting in the first created symbol to this object
						OCADTextSymbol* ocad_text_symbol = (OCADTextSymbol*)ocad_symbol(file, *it);
						setTextSymbolFormatting(ocad_text_symbol, text_object);
						
						TextFormatList new_list;
						new_list.push_back(std::make_pair(text_object, *it));
						text_format_map.insert(text_symbol, new_list);
					}
					else
					{
						// Check if this formatting has already been created as symbol.
						// If yes, use this symbol, else create a new symbol
						TextFormatList& format_list = text_format_map[text_symbol];
						bool found = false;
						for (size_t i = 0, end = format_list.size(); i < end; ++i)
						{
							if (format_list[i].first->getHorizontalAlignment() == text_object->getHorizontalAlignment())
							{
								index_to_use = format_list[i].second;
								found = true;
								break;
							}
						}
						if (!found)
						{
							// Copy the symbol and adjust the formatting
							// TODO: insert these symbols directly after the original symbols
							OCADTextSymbol* ocad_text_symbol = (OCADTextSymbol*)ocad_symbol(file, *it);
							OCADTextSymbol* new_symbol = (OCADTextSymbol*)ocad_symbol_new(file, ocad_text_symbol->size);
							// Get the pointer to the first symbol again as it might have changed during ocad_symbol_new()
							ocad_text_symbol = (OCADTextSymbol*)ocad_symbol(file, *it);
							
							memcpy(new_symbol, ocad_text_symbol, ocad_text_symbol->size);
							setTextSymbolFormatting(new_symbol, text_object);
							
							// Give the new symbol a unique number
							while (symbol_numbers.find(new_symbol->number) != symbol_numbers.end())
								++new_symbol->number;
							symbol_numbers.insert(new_symbol->number);
							index_to_use = new_symbol->number;
							
							format_list.push_back(std::make_pair(text_object, new_symbol->number));
						}
					}
				}
				
				ocad_object->symbol = index_to_use;
				if (object->getType() == Object::Point)
					ocad_object->type = 1;
				else if (object->getType() == Object::Path)
				{
					OCADSymbol* ocad_sym = ocad_symbol(file, index_to_use);
					if (!ocad_sym)
						ocad_object->type = 2;	// This case is for undefined lines; TODO: make another case for undefined areas, as soon as they are implemented
					else if (ocad_sym->type == 2)
						ocad_object->type = 2;	// Line
					else //if (ocad_symbol->type == 3)
						ocad_object->type = 3;	// Area
				}
				else if (object->getType() == Object::Text)
				{
					TextObject* text_object = static_cast<TextObject*>(object);
					if (text_object->hasSingleAnchor())
						ocad_object->type = 4;
					else
						ocad_object->type = 5;
				}
				
				OCADObjectEntry* entry;
				ocad_object_add(file, ocad_object, &entry);
				// This is done internally by libocad (in a slightly more imprecise way using the extent specified in the symbol)
				//entry->rect.min = convertPoint(MapCoordF(object->getExtent().topLeft()).toMapCoord());
				//entry->rect.max = convertPoint(MapCoordF(object->getExtent().bottomRight()).toMapCoord());
				entry->npts = ocad_object->npts + ocad_object->ntext;
				//entry->symbol = index_to_use;
			}
		}
	}
	
	/*
		// TODO
		// Load templates
		map->templates.clear();
		for (OCADStringIndex *idx = ocad_string_index_first(file); idx != NULL; idx = ocad_string_index_next(file, idx))
		{
			for (int i = 0; i < 256; i++)
			{
				OCADStringEntry *entry = ocad_string_entry_at(file, idx, i);
				if (entry->type != 0 && entry->size > 0)
					importString(entry);
			}
		}
	}*/
	
	stream->write((char*)file->buffer, file->size);
	
	ocad_file_close(file);
}

void OCAD8FileExport::exportCommonSymbolFields(Symbol* symbol, OCADSymbol* ocad_symbol, int size)
{
	ocad_symbol->size = (s16)size;
	convertPascalString(symbol->getPlainTextName(), ocad_symbol->name, 32);
	ocad_symbol->number = symbol->getNumberComponent(0) * 10;
	if (symbol->getNumberComponent(1) >= 0)
		ocad_symbol->number += (symbol->getNumberComponent(1) % 10);
	// Ensure uniqueness of the symbol number
	while (symbol_numbers.find(ocad_symbol->number) != symbol_numbers.end())
		++ocad_symbol->number;
	symbol_numbers.insert(ocad_symbol->number);
	
	if (symbol->isProtected())
		ocad_symbol->status |= 1;
	if (symbol->isHidden())
		ocad_symbol->status |= 2;
	
	// Set of used colors
	u8 bitmask = 1;
	u8* bitpos = ocad_symbol->colors;
	for (int c = 0; c < map->getNumColors(); ++c)
	{
		if (symbol->containsColor(map->getColor(c)))
			*bitpos |= bitmask;
		
		bitmask = bitmask << 1;
		if (bitmask == 0)
		{
			bitmask = 1;
			++bitpos;
		}
	}
	
	// Icon: 22x22 with 4 bit color code, origin at bottom left, some padding
	const int icon_size = 22;
	QImage* image = symbol->createIcon(map, icon_size, false, 0);
	u8* ocad_icon = (u8*)ocad_symbol->icon;
	for (int y = icon_size - 1; y >= 0; --y)
	{
		for (int x = 0; x < icon_size; x += 2)
		{
			int first = getOcadColor(image->pixel(x, y));
			int second = getOcadColor(image->pixel(x + 1, y));
			*(ocad_icon++) = (first << 4) + (second);
		}
		ocad_icon++;
	}
	delete image;
}

int OCAD8FileExport::getPatternSize(PointSymbol* point)
{
	if (!point)
		return 0;
	
	int npts = 0;
	for (int i = 0; i < point->getNumElements(); ++i)
	{
		int factor = 1;
		if (point->getElementSymbol(i)->getType() == Symbol::Point)
		{
			factor = 0;
			PointSymbol* point_symbol = static_cast<PointSymbol*>(point->getElementSymbol(i));
			if (point_symbol->getInnerRadius() > 0 && point_symbol->getInnerColor() != NULL)
				++factor;
			if (point_symbol->getOuterWidth() > 0 && point_symbol->getOuterColor() != NULL)
				++factor;
		}
		npts += factor * point->getElementObject(i)->getRawCoordinateVector().size();
		npts += (factor > 0) ? 2 : 0;
	}
	if (point->getInnerRadius() > 0 && point->getInnerColor() != NULL)
		npts += 2 + 1;
	if (point->getOuterWidth() > 0 && point->getOuterColor() != NULL)
		npts += 2 + 1;
	
	return npts * sizeof(OCADPoint);
}

s16 OCAD8FileExport::exportPattern(PointSymbol* point, OCADPoint** buffer)
{
	if (!point)
		return 0;
	
	s16 num_coords = 0;
	for (int i = 0; i < point->getNumElements(); ++i)
		num_coords += exportSubPattern(point->getElementObject(i), point->getElementSymbol(i), buffer);
	num_coords += exportSubPattern(origin_point_object, point, buffer);
	return num_coords;
}

s16 OCAD8FileExport::exportSubPattern(Object* object, Symbol* symbol, OCADPoint** buffer)
{
	s16 num_coords = 0;
	OCADSymbolElement* element = (OCADSymbolElement*)*buffer;
	
	if (symbol->getType() == Symbol::Point)
	{
		PointSymbol* point_symbol = static_cast<PointSymbol*>(symbol);
		if (point_symbol->getInnerRadius() > 0 && point_symbol->getInnerColor() != NULL)
		{
			element->type = 4;
			element->color = convertColor(point_symbol->getInnerColor());
			element->diameter = convertSize(2 * point_symbol->getInnerRadius());
			(*buffer) += 2;
			element->npts = exportCoordinates(object->getRawCoordinateVector(), buffer, point_symbol);
			num_coords += 2 + element->npts;
		}
		if (point_symbol->getOuterWidth() > 0 && point_symbol->getOuterColor() != NULL)
		{
			element = (OCADSymbolElement*)*buffer;
			element->type = 3;
			element->color = convertColor(point_symbol->getOuterColor());
			element->width = convertSize(point_symbol->getOuterWidth());
			element->diameter = convertSize(2 * point_symbol->getInnerRadius() + 2 * point_symbol->getOuterWidth());
			(*buffer) += 2;
			element->npts = exportCoordinates(object->getRawCoordinateVector(), buffer, point_symbol);
			num_coords += 2 + element->npts;
		}
	}
	else if (symbol->getType() == Symbol::Line)
	{
		LineSymbol* line_symbol = static_cast<LineSymbol*>(symbol);
		element->type = 1;
		if (line_symbol->getCapStyle() == LineSymbol::RoundCap)
			element->flags |= 1;
		else if (line_symbol->getJoinStyle() == LineSymbol::MiterJoin)
			element->flags |= 4;
		element->color = convertColor(line_symbol->getColor());
		element->width = convertSize(line_symbol->getLineWidth());
		(*buffer) += 2;
		element->npts = exportCoordinates(object->getRawCoordinateVector(), buffer, line_symbol);
		num_coords += 2 + element->npts;
	}
	else if (symbol->getType() == Symbol::Area)
	{
		AreaSymbol* area_symbol = static_cast<AreaSymbol*>(symbol);
		element->type = 2;
		element->color = convertColor(area_symbol->getColor());
		(*buffer) += 2;
		element->npts = exportCoordinates(object->getRawCoordinateVector(), buffer, area_symbol);
		num_coords += 2 + element->npts;
	}
	else
		assert(false);
	return num_coords;
}

s16 OCAD8FileExport::exportPointSymbol(PointSymbol* point)
{
	int data_size = (sizeof(OCADPointSymbol) - sizeof(OCADPoint)) + getPatternSize(point);
	OCADPointSymbol* ocad_symbol = (OCADPointSymbol*)ocad_symbol_new(file, data_size);
	exportCommonSymbolFields(point, (OCADSymbol*)ocad_symbol, data_size);
	
	ocad_symbol->type = OCAD_POINT_SYMBOL;
	ocad_symbol->extent = getPointSymbolExtent(point);
	if (ocad_symbol->extent <= 0)
		ocad_symbol->extent = 100;
	if (point->isRotatable())
		ocad_symbol->base_flags |= 1;
	ocad_symbol->ngrp = (data_size - (sizeof(OCADPointSymbol) - sizeof(OCADPoint))) / 8;
	
	OCADPoint* pattern_buffer = ocad_symbol->pts;
	exportPattern(point, &pattern_buffer);
	assert((u8*)ocad_symbol + data_size == (u8*)pattern_buffer);
	return ocad_symbol->number;
}

s16 OCAD8FileExport::exportLineSymbol(LineSymbol* line)
{
	int data_size = (sizeof(OCADLineSymbol) - sizeof(OCADPoint)) +
					getPatternSize(line->getStartSymbol()) +
					getPatternSize(line->getEndSymbol()) +
					getPatternSize(line->getMidSymbol()) +
					getPatternSize(line->getDashSymbol());
	OCADLineSymbol* ocad_symbol = (OCADLineSymbol*)ocad_symbol_new(file, data_size);
	exportCommonSymbolFields(line, (OCADSymbol*)ocad_symbol, data_size);
	
	// Basic settings
	ocad_symbol->type = OCAD_LINE_SYMBOL;
	s16 extent = convertSize(0.5f * line->getLineWidth());
	if (line->hasBorder())
		extent = qMax(extent, (s16)convertSize(0.5f * line->getLineWidth() + line->getBorderShift() + 0.5f * line->getBorderLineWidth()));
	extent = qMax(extent, getPointSymbolExtent(line->getStartSymbol()));
	extent = qMax(extent, getPointSymbolExtent(line->getEndSymbol()));
	extent = qMax(extent, getPointSymbolExtent(line->getMidSymbol()));
	extent = qMax(extent, getPointSymbolExtent(line->getDashSymbol()));
	ocad_symbol->extent = extent;
	ocad_symbol->color = convertColor(line->getColor());
	if (line->getColor() != NULL)
		ocad_symbol->width = convertSize(line->getLineWidth());
	
	// Cap and Join
	if (line->getCapStyle() == LineSymbol::FlatCap && line->getJoinStyle() == LineSymbol::BevelJoin)
		ocad_symbol->ends = 0;
	else if (line->getCapStyle() == LineSymbol::RoundCap && line->getJoinStyle() == LineSymbol::RoundJoin)
		ocad_symbol->ends = 1;
	else if (line->getCapStyle() == LineSymbol::PointedCap && line->getJoinStyle() == LineSymbol::BevelJoin)
		ocad_symbol->ends = 2;
	else if (line->getCapStyle() == LineSymbol::PointedCap && line->getJoinStyle() == LineSymbol::RoundJoin)
		ocad_symbol->ends = 3;
	else if (line->getCapStyle() == LineSymbol::FlatCap && line->getJoinStyle() == LineSymbol::MiterJoin)
		ocad_symbol->ends = 4;
	else if (line->getCapStyle() == LineSymbol::PointedCap && line->getJoinStyle() == LineSymbol::MiterJoin)
		ocad_symbol->ends = 6;
	else
	{
		addWarning(QObject::tr("In line symbol \"%1\", cannot represent cap/join combination.").arg(line->getPlainTextName()));
		// Decide based on the caps
		if (line->getCapStyle() == LineSymbol::FlatCap)
			ocad_symbol->ends = 0;
		else if (line->getCapStyle() == LineSymbol::RoundCap)
			ocad_symbol->ends = 1;
		else if (line->getCapStyle() == LineSymbol::PointedCap)
			ocad_symbol->ends = 3;
		else if (line->getCapStyle() == LineSymbol::SquareCap)
			ocad_symbol->ends = 0;
	}
	
	if (line->getCapStyle() == LineSymbol::PointedCap)
	{
		ocad_symbol->bdist = convertSize(line->getPointedCapLength());
		ocad_symbol->edist = convertSize(line->getPointedCapLength());
	}
	
	// Dash pattern
	if (line->isDashed())
	{
		if (line->getMidSymbol() != NULL && !line->getMidSymbol()->isEmpty())
		{
			if (line->getDashesInGroup() > 1)
				addWarning(QObject::tr("In line symbol \"%1\", neglecting the dash grouping.").arg(line->getPlainTextName()));
			
			ocad_symbol->len = convertSize(line->getDashLength() + line->getBreakLength());
			ocad_symbol->elen = ocad_symbol->len / 2;
			ocad_symbol->gap2 = convertSize(line->getBreakLength());
		}
		else
		{
			if (line->getDashesInGroup() > 1)
			{
				if (line->getDashesInGroup() > 2)
					addWarning(QObject::tr("In line symbol \"%1\", the number of dashes in a group has been reduced to 2.").arg(line->getPlainTextName()));
				
				ocad_symbol->len = convertSize(2 * line->getDashLength() + line->getInGroupBreakLength());
				ocad_symbol->elen = convertSize(2 * line->getDashLength() + line->getInGroupBreakLength());
				ocad_symbol->gap = convertSize(line->getBreakLength());
				ocad_symbol->gap2 = convertSize(line->getInGroupBreakLength());
				ocad_symbol->egap = ocad_symbol->gap2;
			}
			else
			{
				ocad_symbol->len = convertSize(line->getDashLength());
				ocad_symbol->elen = ocad_symbol->len / (line->getHalfOuterDashes() ? 2 : 1);
				ocad_symbol->gap = convertSize(line->getBreakLength());
			}
		}
	}
	else
	{
		ocad_symbol->len = convertSize(line->getSegmentLength());
		ocad_symbol->elen = convertSize(line->getEndLength());
	}
	
	ocad_symbol->smin = line->getShowAtLeastOneSymbol() ? 0 : -1;
	
	// Double line
	if (line->hasBorder() && line->getBorderColor() != NULL)
	{
		ocad_symbol->dwidth = convertSize(line->getLineWidth() - line->getBorderLineWidth() + 2 * line->getBorderShift());
		ocad_symbol->dmode = line->isBorderDashed() ? 3 : 1;
		/*if (!line->isDashed())
		{
			// Use the "fill" option instead of the main line so the import works better
			ocad_symbol->dcolor = ocad_symbol->color;
			ocad_symbol->width = 0;
		}*/
		// ocad_symbol->dflags
		
		ocad_symbol->lwidth = convertSize(line->getBorderLineWidth());
		ocad_symbol->rwidth = ocad_symbol->lwidth;
		
		ocad_symbol->lcolor = convertColor(line->getBorderColor());
		ocad_symbol->rcolor = ocad_symbol->lcolor;
		
		if (line->isBorderDashed())
		{
			ocad_symbol->dlen = convertSize(line->getBorderDashLength());
			ocad_symbol->dgap = convertSize(line->getBorderBreakLength());
		}
	}
	
	// Mid symbol
	OCADPoint* pattern_buffer = ocad_symbol->pts;
	ocad_symbol->smnpts = exportPattern(line->getMidSymbol(), &pattern_buffer);
	ocad_symbol->snum = line->getMidSymbolsPerSpot();
	ocad_symbol->sdist = convertSize(line->getMidSymbolDistance());
	
	// No secondary symbol
	ocad_symbol->ssnpts = 0;
	
	// Export dash symbol as corner symbol
	ocad_symbol->scnpts = exportPattern(line->getDashSymbol(), &pattern_buffer);
	
	// Start symbol
	ocad_symbol->sbnpts = exportPattern(line->getStartSymbol(), &pattern_buffer);
	
	// End symbol
	ocad_symbol->senpts = exportPattern(line->getEndSymbol(), &pattern_buffer);
	
	assert((u8*)ocad_symbol + data_size == (u8*)pattern_buffer);
	return ocad_symbol->number;
}

s16 OCAD8FileExport::exportAreaSymbol(AreaSymbol* area)
{
	int data_size = (sizeof(OCADAreaSymbol) - sizeof(OCADPoint));
	for (int i = 0, end = area->getNumFillPatterns(); i < end; ++i)
	{
		if (area->getFillPattern(i).type == AreaSymbol::FillPattern::PointPattern)
		{
			data_size += getPatternSize(area->getFillPattern(i).point);
			break;
		}
	}
	OCADAreaSymbol* ocad_symbol = (OCADAreaSymbol*)ocad_symbol_new(file, data_size);
	exportCommonSymbolFields(area, (OCADSymbol*)ocad_symbol, data_size);
	
	// Basic settings
	ocad_symbol->type = OCAD_AREA_SYMBOL;
	ocad_symbol->extent = 0;
	if (area->getColor() != NULL)
	{
		ocad_symbol->fill = 1;
		ocad_symbol->color = convertColor(area->getColor());
	}
	
	// Hatch
	for (int i = 0, end = area->getNumFillPatterns(); i < end; ++i)
	{
		AreaSymbol::FillPattern& pattern = area->getFillPattern(i);
		if (pattern.type == AreaSymbol::FillPattern::LinePattern)
		{
			if (ocad_symbol->hmode == 1 && ocad_symbol->hcolor != convertColor(pattern.line_color))
			{
				addWarning(QObject::tr("In area symbol \"%1\", skipping a fill pattern.").arg(area->getPlainTextName()));
				continue;
			}
			
			if (pattern.rotatable)
				ocad_symbol->base_flags |= 1;
			
			++ocad_symbol->hmode;
			if (ocad_symbol->hmode == 1)
			{
				ocad_symbol->hcolor = convertColor(pattern.line_color);
				ocad_symbol->hwidth = convertSize(pattern.line_width);
				ocad_symbol->hdist = convertSize(pattern.line_spacing - pattern.line_width);
				ocad_symbol->hangle1 = convertRotation(pattern.angle);
			}
			else if (ocad_symbol->hmode == 2)
			{
				ocad_symbol->hwidth = (ocad_symbol->hwidth + convertSize(pattern.line_width)) / 2;
				ocad_symbol->hdist = (ocad_symbol->hdist + convertSize(pattern.line_spacing - pattern.line_width)) / 2;
				ocad_symbol->hangle2 = convertRotation(pattern.angle);
				
				// No futher hatch pattern supported by .ocd version 8
				break;
			}
		}
	}
	
	// Struct
	PointSymbol* point_pattern = NULL;
	for (int i = 0, end = area->getNumFillPatterns(); i < end; ++i)
	{
		AreaSymbol::FillPattern& pattern = area->getFillPattern(i);
		if (pattern.type == AreaSymbol::FillPattern::PointPattern)
		{
			if (pattern.rotatable)
				ocad_symbol->base_flags |= 1;
			
			++ocad_symbol->pmode;
			if (ocad_symbol->pmode == 1)
			{
				ocad_symbol->pwidth = convertSize(pattern.point_distance);
				ocad_symbol->pheight = convertSize(pattern.line_spacing);
				ocad_symbol->pangle = convertRotation(pattern.angle);
				point_pattern = pattern.point;
			}
			else if (ocad_symbol->pmode == 2)
			{
				// NOTE: This is only a heuristic which works for the orienteering symbol sets, not a real conversion, which would be impossible in most cases.
				//       There are no further checks done to find out if the conversion is applicable because with these checks, already a tiny (not noticeable) error
				//       in the symbol definition would make it take the wrong choice.
				addWarning(QObject::tr("In area symbol \"%1\", assuming a \"shifted rows\" point pattern. This might be correct as well as incorrect.").arg(area->getPlainTextName()));
				
				if (pattern.line_offset != 0)
					ocad_symbol->pheight /= 2;
				else
					ocad_symbol->pwidth /= 2;
				
				break;
			}
		}
	}
	
	if (point_pattern)
	{
		OCADPoint* pattern_buffer = ocad_symbol->pts;
		ocad_symbol->npts = exportPattern(point_pattern, &pattern_buffer);
		assert((u8*)ocad_symbol + data_size == (u8*)pattern_buffer);
	}
	return ocad_symbol->number;
}

s16 OCAD8FileExport::exportTextSymbol(TextSymbol* text)
{
	int data_size = sizeof(OCADTextSymbol);
	OCADTextSymbol* ocad_symbol = (OCADTextSymbol*)ocad_symbol_new(file, data_size);
	exportCommonSymbolFields(text, (OCADSymbol*)ocad_symbol, data_size);
	
	ocad_symbol->type = OCAD_TEXT_SYMBOL;
	ocad_symbol->subtype = 1;
	ocad_symbol->extent = 0;
	
	convertPascalString(text->getFontFamily(), ocad_symbol->font, 32);
	ocad_symbol->color = convertColor(text->getColor());
	ocad_symbol->dpts = qRound(10 * text->getFontSize() / 25.4 * 72.0);
	ocad_symbol->bold = text->isBold() ? 700 : 400;
	ocad_symbol->italic = text->isItalic() ? 1 : 0;
	//ocad_symbol->charset
	ocad_symbol->cspace = convertSize(1000 * text->getCharacterSpacing());
	if (ocad_symbol->cspace != 0)
		addWarning(QObject::tr("In text symbol %1: custom character spacing is set, its implementation does not match OCAD's behavior yet").arg(text->getPlainTextName()));
	ocad_symbol->wspace = 100;
	ocad_symbol->halign = 0;	// Default value, we might have to change this or even create copies of this symbol with other alignments later
	double absolute_line_spacing = text->getLineSpacing() * (text->getFontMetrics().lineSpacing() / text->calculateInternalScaling());
	ocad_symbol->lspace = qRound(absolute_line_spacing / (text->getFontSize() * 0.01));
	ocad_symbol->pspace = convertSize(1000 * text->getParagraphSpacing());
	if (text->isUnderlined())
		addWarning(QObject::tr("In text symbol %1: ignoring underlining").arg(text->getPlainTextName()));
	if (text->usesKerning())
		addWarning(QObject::tr("In text symbol %1: ignoring kerning").arg(text->getPlainTextName()));
	
	ocad_symbol->under = text->hasLineBelow() ? 1 : 0;
	ocad_symbol->ucolor = convertColor(text->getLineBelowColor());
	ocad_symbol->uwidth = convertSize(1000 * text->getLineBelowWidth());
	ocad_symbol->udist = convertSize(1000 * text->getLineBelowDistance());
	
	ocad_symbol->ntabs = text->getNumCustomTabs();
	for (int i = 0; i < qMin((s16)32, ocad_symbol->ntabs); ++i)
		ocad_symbol->tab[i] = convertSize(text->getCustomTab(i));
	
	if (text->getFramingMode() != TextSymbol::NoFraming && text->getFramingColor() != NULL)
	{
		ocad_symbol->fcolor = convertColor(text->getFramingColor());
		if (text->getFramingMode() == TextSymbol::ShadowFraming)
		{
			ocad_symbol->fmode = 1;
			ocad_symbol->fdx = convertSize(text->getFramingShadowXOffset());
			ocad_symbol->fdy = -1 * convertSize(text->getFramingShadowYOffset());
		}
		else if (text->getFramingMode() == TextSymbol::LineFraming)
		{
			ocad_symbol->fmode = 2;
			ocad_symbol->fdpts = convertSize(text->getFramingLineHalfWidth());
		}
		else
			assert(false);
	}
	
	return ocad_symbol->number;
}

void OCAD8FileExport::setTextSymbolFormatting(OCADTextSymbol* ocad_symbol, TextObject* formatting)
{
	if (formatting->getHorizontalAlignment() == TextObject::AlignLeft)
		ocad_symbol->halign = 0;
	else if (formatting->getHorizontalAlignment() == TextObject::AlignHCenter)
		ocad_symbol->halign = 1;
	else if (formatting->getHorizontalAlignment() == TextObject::AlignRight)
		ocad_symbol->halign = 2;
}

std::set< s16 > OCAD8FileExport::exportCombinedSymbol(CombinedSymbol* combination)
{
	std::vector<bool> map_bitfield;
	map_bitfield.assign(map->getNumSymbols(), false);
	map_bitfield[map->findSymbolIndex(combination)] = true;
	map->determineSymbolUseClosure(map_bitfield);
	
	std::set<s16> result;
	for (size_t i = 0, end = map_bitfield.size(); i < end; ++i)
	{
		if (map_bitfield[i] && symbol_index.contains(map->getSymbol(i)))
		{
			result.insert(symbol_index[map->getSymbol(i)].begin(),
			              symbol_index[map->getSymbol(i)].end());
		}
	}
	return result;
}

s16 OCAD8FileExport::exportCoordinates(const MapCoordVector& coords, OCADPoint** buffer, Symbol* symbol)
{
	s16 num_points = 0;
	bool curve_start = false;
	bool hole_point = false;
	bool curve_continue = false;
	for (size_t i = 0, end = coords.size(); i < end; ++i)
	{
		const MapCoord& point = coords[i];
		OCADPoint p;
		p.x = (point.rawX() / 10) << 8;
		p.y = (point.rawY() / -10) << 8;
		
		if (point.isDashPoint())
		{
			if (symbol == NULL || symbol->getType() != Symbol::Line)
				p.y |= PY_CORNER;
			else
			{
				LineSymbol* line_symbol = static_cast<LineSymbol*>(symbol);
				if ((line_symbol->getDashSymbol() == NULL || line_symbol->getDashSymbol()->isEmpty()) && line_symbol->isDashed())
					p.y |= PY_DASH;
				else
					p.y |= PY_CORNER;
			}
		}
		if (curve_start)
			p.x |= PX_CTL1;
		if (hole_point)
			p.y |= PY_HOLE;
		if (curve_continue)
			p.x |= PX_CTL2;
		
		curve_continue = curve_start;
		curve_start = point.isCurveStart();
		hole_point = point.isHolePoint();
		
		**buffer = p;
		++(*buffer);
		++num_points;
	}
	return num_points;
}

s16 OCAD8FileExport::exportTextCoordinates(TextObject* object, OCADPoint** buffer)
{
	if (object->getNumLines() == 0)
		return 0;
	
	QTransform text_to_map = object->calcTextToMapTransform();
	QTransform map_to_text = object->calcMapToTextTransform();
	
	if (object->hasSingleAnchor())
	{
		// Create 5 coordinates:
		// 0 - baseline anchor point
		// 1 - bottom left
		// 2 - bottom right
		// 3 - top right
		// 4 - top left
		
		QPointF anchor = object->getAnchorCoordF().toQPointF();
		QPointF anchor_text = map_to_text.map(anchor);
		
		TextObjectLineInfo* line0 = object->getLineInfo(0);
		**buffer = convertPoint(MapCoordF(text_to_map.map(QPointF(anchor_text.x(), line0->line_y))).toMapCoord());
		++(*buffer);
		
		QRectF bounding_box_text;
		for (int i = 0; i < object->getNumLines(); ++i)
		{
			TextObjectLineInfo* info = object->getLineInfo(i);
			rectIncludeSafe(bounding_box_text, QPointF(info->line_x, info->line_y - info->ascent));
			rectIncludeSafe(bounding_box_text, QPointF(info->line_x + info->width, info->line_y + info->descent));
		}
		
		**buffer = convertPoint(MapCoordF(text_to_map.map(bounding_box_text.bottomLeft())).toMapCoord());
		++(*buffer);
		**buffer = convertPoint(MapCoordF(text_to_map.map(bounding_box_text.bottomRight())).toMapCoord());
		++(*buffer);
		**buffer = convertPoint(MapCoordF(text_to_map.map(bounding_box_text.topRight())).toMapCoord());
		++(*buffer);
		**buffer = convertPoint(MapCoordF(text_to_map.map(bounding_box_text.topLeft())).toMapCoord());
		++(*buffer);
		
		return 5;
	}
	else
	{
		// As OCD 8 only supports Top alignment, we have to replace the top box coordinates by the top coordinates of the first line
		TextSymbol* text_symbol = static_cast<TextSymbol*>(object->getSymbol());
		QFontMetricsF metrics = text_symbol->getFontMetrics();
		double internal_scaling = text_symbol->calculateInternalScaling();
		TextObjectLineInfo* line0 = object->getLineInfo(0);
		
		double new_top = line0->line_y - line0->ascent;
		// Account for extra internal leading
		double top_adjust = -text_symbol->getFontSize() * internal_scaling + (metrics.ascent() + metrics.descent() + 0.5);
		new_top = (new_top - top_adjust) / internal_scaling;
		
		QTransform transform;
		transform.rotate(-object->getRotation() * 180 / M_PI);
		**buffer = convertPoint((MapCoordF(transform.map(QPointF(-object->getBoxWidth() / 2, object->getBoxHeight() / 2))) + object->getAnchorCoordF()).toMapCoord());
		++(*buffer);
		**buffer = convertPoint((MapCoordF(transform.map(QPointF(object->getBoxWidth() / 2, object->getBoxHeight() / 2))) + object->getAnchorCoordF()).toMapCoord());
		++(*buffer);
		**buffer = convertPoint((MapCoordF(transform.map(QPointF(object->getBoxWidth() / 2, new_top))) + object->getAnchorCoordF()).toMapCoord());
		++(*buffer);
		**buffer = convertPoint((MapCoordF(transform.map(QPointF(-object->getBoxWidth() / 2, new_top))) + object->getAnchorCoordF()).toMapCoord());
		++(*buffer);
		
		return 4;
	}
}

int OCAD8FileExport::getOcadColor(QRgb rgb)
{
	// Simple comparison function which takes the best matching color.
	static const QColor ocad_colors[16] = {
		QColor(  0,   0,   0).toHsv(),
		QColor(128,   0,   0).toHsv(),
		QColor(0,   128,   0).toHsv(),
		QColor(128, 128,   0).toHsv(),
		QColor(  0,   0, 128).toHsv(),
		QColor(128,   0, 128).toHsv(),
		QColor(  0, 128, 128).toHsv(),
		QColor(128, 128, 128).toHsv(),
		QColor(192, 192, 192).toHsv(),
		QColor(255,   0,   0).toHsv(),
		QColor(  0, 255,   0).toHsv(),
		QColor(255, 255,   0).toHsv(),
		QColor(  0,   0, 255).toHsv(),
		QColor(255,   0, 255).toHsv(),
		QColor(  0, 255, 255).toHsv(),
		QColor(255, 255, 255).toHsv()
	};
	
	// Return white for transparent areas
	if (qAlpha(rgb) < 128)
		return 15;
	
	QColor color = QColor(rgb).toHsv();
	int best_index = 0;
	float best_distance = 999999;
	for (int i = 0; i < 16; ++i)
	{
		int hue_dist = qAbs(color.hue() - ocad_colors[i].hue());
		hue_dist = qMin(hue_dist, 360 - hue_dist);
		float distance = qPow(hue_dist, 2) + 
						  0.1f * qPow(color.saturation() - ocad_colors[i].saturation(), 2) +
						  0.1f * qPow(color.value() - ocad_colors[i].value(), 2);
		
		// (Too much) manual tweaking for orienteering colors
		if (i == 1)
			distance *= 1.5;	// Dark red
		else if (i == 3)
			distance *= 2;		// Olive
		else if (i == 7)
			distance *= 2;		// Dark gray
		else if (i == 8)
			distance *= 3;		// Light gray
		else if (i == 11)
			distance *= 2;		// Yellow
		else if (i == 9)
			distance *= 3;		// Red is unlikely
		else if (i == 15)
			distance *= 4;		// White is very unlikely
			
		if (distance < best_distance)
		{
			best_distance = distance;
			best_index = i;
		}
	}
	return best_index;
}

s16 OCAD8FileExport::getPointSymbolExtent(PointSymbol* symbol)
{
	if (!symbol)
		return 0;
	QRectF extent;
	for (int i = 0; i < symbol->getNumElements(); ++i)
	{
		Object* object = symbol->getElementObject(i);
		Symbol* old_symbol = object->getSymbol();
		object->setSymbol(symbol->getElementSymbol(i), true);
		object->update(true, false);
		
		rectIncludeSafe(extent, symbol->getElementObject(i)->getExtent());
		
		object->setSymbol(old_symbol, true);
	}
	float float_extent = 0.5f * qMax(extent.width(), extent.height());
	if (symbol->getInnerColor() != NULL)
		float_extent = qMax(float_extent, 0.001f * symbol->getInnerRadius());
	if (symbol->getOuterColor() != NULL)
		float_extent = qMax(float_extent, 0.001f * (symbol->getInnerRadius() + symbol->getOuterWidth()));
	return convertSize(1000 * float_extent);
}

void OCAD8FileExport::convertPascalString(const QString& text, char* buffer, int buffer_size)
{
	assert(buffer_size <= 256);		// not possible to store a bigger length in the first byte
	int max_size = buffer_size - 1;
	
	if (text.length() > max_size)
		addStringTruncationWarning(text, max_size);
	
	QByteArray data = encoding_1byte->fromUnicode(text);
	int min_size = qMin(text.length(), max_size);
	*((unsigned char *)buffer) = min_size;
	mempcpy(buffer + 1, data.data(), min_size);
}

void OCAD8FileExport::convertCString(const QString& text, unsigned char* buffer, int buffer_size)
{
	if (text.length() + 1 > buffer_size)
		addStringTruncationWarning(text, buffer_size - 1);
	
	QByteArray data = encoding_1byte->fromUnicode(text);
	int min_size = qMin(buffer_size - 1, data.length());
	mempcpy(buffer, data.data(), min_size);
	buffer[min_size] = 0;
}

int OCAD8FileExport::convertWideCString(const QString& text, unsigned char* buffer, int buffer_size)
{
	// Convert text to Windows-OCAD format:
	// - if it starts with a newline, add another
	// - convert \n to \r\n
	QString exported_text;
	if (text.startsWith('\n'))
		exported_text = "\n" + text;
	else
		exported_text = text;
	exported_text.replace('\n', "\r\n");
	
	if (2 * (exported_text.length() + 1) > buffer_size)
		addStringTruncationWarning(exported_text, buffer_size - 1);
	
	// Do not add a byte order mark by using QTextCodec::IgnoreHeader
	QTextEncoder* encoder = encoding_2byte->makeEncoder(QTextCodec::IgnoreHeader);
	QByteArray data = encoder->fromUnicode(exported_text);
	delete encoder;
	
	int min_size = qMin(buffer_size - 2, data.length());
	mempcpy(buffer, data.data(), min_size);
	buffer[min_size] = 0;
	buffer[min_size + 1] = 0;
	return min_size + 2;
}

int OCAD8FileExport::convertRotation(float angle)
{
	return qRound(10 * (angle * 180 / M_PI));
}

OCADPoint OCAD8FileExport::convertPoint(qint64 x, qint64 y)
{
	OCADPoint result;
	result.x = (s32)(x / 10) << 8;
	result.y = (s32)(y / -10) << 8;
	return result;
}
OCADPoint OCAD8FileExport::convertPoint(const MapCoord& coord)
{
	return convertPoint(coord.rawX(), coord.rawY());
}

s32 OCAD8FileExport::convertSize(qint64 size)
{
	return (s32)(size / 10);
}

s16 OCAD8FileExport::convertColor(MapColor* color)
{
	int index = map->findColorIndex(color);
	return (index > 0) ? index : 0;
}

void OCAD8FileExport::addStringTruncationWarning(const QString& text, int truncation_pos)
{
	QString temp = text;
	temp.insert(truncation_pos, "|||");
	addWarning(QObject::tr("String truncated (truncation marked with three '|'): %1").arg(temp));
}
/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Descriptor.cpp
 * Holds the metadata (schema) for a Message.
 * Copyright (C) 2011 Simon Newton
 */


#include <ola/messaging/Descriptor.h>
#include <ola/messaging/SchemaPrinterVisitor.h>
#include <iostream>
#include <string>

namespace ola {
namespace messaging {


using std::string;
using std::endl;


void SchemaPrinterVisitor::Visit(const BoolFieldDescriptor *descriptor) {
  std::cout << m_indent << std::endl;
  m_str << string(m_indent, ' ') << descriptor->Name() << ": bool" << endl;
}


void SchemaPrinterVisitor::Visit(const StringFieldDescriptor *descriptor) {
  m_str << string(m_indent, ' ') << descriptor->Name() << ": string [" <<
    descriptor->MinSize() << ", " << descriptor->MaxSize() << "]" << endl;
}


void SchemaPrinterVisitor::Visit(
    const IntegerFieldDescriptor<uint8_t> *descriptor) {
  m_str << string(m_indent, ' ') << descriptor->Name() << ": uint8";

  m_str << endl;
}


void SchemaPrinterVisitor::Visit(
    const IntegerFieldDescriptor<uint16_t> *descriptor) {
  m_str << string(m_indent, ' ') << descriptor->Name() << ": uint16";
  m_str << endl;
}


void SchemaPrinterVisitor::Visit(
    const IntegerFieldDescriptor<uint32_t> *descriptor) {
  m_str << string(m_indent, ' ') << descriptor->Name() << ": uint32";
  m_str << endl;
}


void SchemaPrinterVisitor::Visit(
    const IntegerFieldDescriptor<int8_t> *descriptor) {
  m_str << string(m_indent, ' ') << descriptor->Name() << ": int8";
  m_str << endl;
}


void SchemaPrinterVisitor::Visit(
    const IntegerFieldDescriptor<int16_t> *descriptor) {
  m_str << string(m_indent, ' ') << descriptor->Name() << ": int16";
  m_str << endl;
}


void SchemaPrinterVisitor::Visit(
    const IntegerFieldDescriptor<int32_t> *descriptor) {
  m_str << string(m_indent, ' ') << descriptor->Name() << ": int32";
  m_str << endl;
}

void SchemaPrinterVisitor::Visit(const GroupFieldDescriptor *descriptor) {
  m_str << string(m_indent, ' ') << descriptor->Name() << " {" << endl;
  m_indent += m_indent_size;
}


void SchemaPrinterVisitor::PostVisit(const GroupFieldDescriptor *descriptor) {
  m_indent -= m_indent_size;
  m_str << string(m_indent, ' ') << "}" << endl;
  (void) descriptor;
}
}  // messaging
}  // ola

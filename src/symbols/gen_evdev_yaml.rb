#!/usr/bin/ruby -w

def gen_event_list(regex, prefix)
  ret = {}

  File.new("/usr/include/linux/input.h").grep(regex).each do |i|
    name, value = i.split[1..2]
    if not name =~ /_MAX$/ and not name =~ /_CNT$/ 
      begin
        int_val = Integer(value)
        ret[int_val] = [] if not ret.has_key?(int_val)
        ret[int_val].push(name)
        ret[int_val].push("#{prefix}#{int_val}")
      rescue Exception => e  
        # assume we have an alias

        # get the value of the alias
        k, v = ret.find{|el| 
          # puts "#{el[1].inspect}  #{name}  #{el[1].member?(value)}"
          el[1].member?(value)
        }
        ret[k].push(name)
      end
    end
  end

  return ret
end

key = gen_event_list(/^#define (BTN|KEY)/, "KEY_#")
rel = gen_event_list(/^#define REL/, "KEY_#")
abs = gen_event_list(/^#define ABS/, "KEY_#")

def print_name(fout, syms)
  syms.each do |k,v|
    fout.puts "  - name: '#{v[0]}'"
    aliases = v[1..-1].uniq
    fout.puts "    aliases: [#{aliases.map{|l| "'#{l}'"}.join(', ')}]" if not aliases.empty?
    fout.puts 
  end
end

File.open("evdev.yaml", "w") do |fout|
  fout.puts "# automatically generated by #{$0}"
  fout.puts "---"
  fout.puts "- name: 'evdev'"
  fout.puts "  aliases: ['ev']"
  fout.puts "  abs:"
  print_name(fout, abs)

  fout.puts "  key:"
  print_name(fout, key)

  fout.puts "  rel:"
  print_name(fout, rel)

  fout.puts "..."
end

# EOF #
